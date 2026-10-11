#include "flac_codec.h"
#include "flac_native_writer.h"
#include "flac_primitives.h"
#include "hash.h"
#include "lds_codec.h"
#include "native_flac_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::int32_t> make_samples()
{
    std::vector<std::int32_t> samples;
    for (int i = 0; i < 16; ++i) {
        samples.push_back((((i * 7) % 1024) - 512) * 64);
    }
    return samples;
}

std::vector<std::int32_t> make_lpc_friendly_samples()
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<std::int32_t> samples;
    samples.reserve(512);
    for (int i = 0; i < 512; ++i) {
        const double sample =
            (std::sin((2.0 * kPi * i) / 31.0) * 11000.0) +
            (std::sin((2.0 * kPi * i) / 11.0) * 3500.0);
        auto quantized = static_cast<int>(std::lround(sample / 64.0)) * 64;
        quantized = std::clamp(quantized, -32768, 32704);
        samples.push_back(quantized);
    }
    return samples;
}

std::string pack_expected_lds(const std::vector<std::int32_t>& samples)
{
    std::string packed;
    for (std::size_t i = 0; i < samples.size(); i += 4) {
        const ldcompress::SampleGroup group {
            static_cast<std::int16_t>(samples[i + 0]),
            static_cast<std::int16_t>(samples[i + 1]),
            static_cast<std::int16_t>(samples[i + 2]),
            static_cast<std::int16_t>(samples[i + 3]),
        };
        const auto packed_group = ldcompress::pack_group(group);
        packed.append(reinterpret_cast<const char*>(packed_group.data()), packed_group.size());
    }
    return packed;
}

std::array<std::uint8_t, 16> md5_samples_s16le(const std::vector<std::int32_t>& samples)
{
    ldcompress::Md5 md5;
    for (const auto sample : samples) {
        const auto s16 = static_cast<std::int16_t>(sample);
        const std::array<std::uint8_t, 2> bytes {
            static_cast<std::uint8_t>(s16 & 0xff),
            static_cast<std::uint8_t>((s16 >> 8) & 0xff),
        };
        md5.update(bytes.data(), bytes.size());
    }
    return md5.digest();
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

unsigned read_bits(std::string_view bytes, std::size_t bit_offset, unsigned bit_count)
{
    if (bit_count > 32) {
        throw std::runtime_error("test bit reader cannot return more than 32 bits");
    }
    unsigned value = 0;
    for (unsigned i = 0; i < bit_count; ++i) {
        const auto absolute_bit = bit_offset + i;
        const auto byte = static_cast<unsigned char>(bytes.at(absolute_bit / 8));
        const auto bit = (byte >> (7U - (absolute_bit % 8U))) & 1U;
        value = (value << 1U) | bit;
    }
    return value;
}

std::uint64_t read_bits64(std::string_view bytes, std::size_t bit_offset, unsigned bit_count)
{
    if (bit_count > 64) {
        throw std::runtime_error("test bit reader cannot return more than 64 bits");
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bit_count; ++i) {
        const auto absolute_bit = bit_offset + i;
        const auto byte = static_cast<unsigned char>(bytes.at(absolute_bit / 8));
        const auto bit = (byte >> (7U - (absolute_bit % 8U))) & 1U;
        value = (value << 1U) | bit;
    }
    return value;
}

unsigned read_u16be(std::string_view bytes, std::size_t offset)
{
    return (static_cast<unsigned>(static_cast<unsigned char>(bytes.at(offset))) << 8U) |
        static_cast<unsigned>(static_cast<unsigned char>(bytes.at(offset + 1U)));
}

unsigned read_u24be(std::string_view bytes, std::size_t offset)
{
    return (static_cast<unsigned>(static_cast<unsigned char>(bytes.at(offset))) << 16U) |
        (static_cast<unsigned>(static_cast<unsigned char>(bytes.at(offset + 1U))) << 8U) |
        static_cast<unsigned>(static_cast<unsigned char>(bytes.at(offset + 2U)));
}

std::uint8_t byte_at(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint8_t>(static_cast<unsigned char>(bytes.at(offset)));
}

struct FirstFrameFixedInfo {
    unsigned order = 0;
    unsigned partition_order = 0;
    unsigned wasted_bits = 0;
};

struct FirstFrameSubframeInfo {
    unsigned type = 0;
    unsigned wasted_bits = 0;
    std::size_t sample_data_bit_offset = 0;
};

struct NativeStreamInfo {
    bool is_last_metadata_block = false;
    unsigned metadata_type = 0;
    unsigned metadata_length = 0;
    unsigned min_block_size = 0;
    unsigned max_block_size = 0;
    unsigned min_frame_size = 0;
    unsigned max_frame_size = 0;
    unsigned sample_rate = 0;
    unsigned channels = 0;
    unsigned bits_per_sample = 0;
    std::uint64_t total_samples = 0;
    std::array<std::uint8_t, 16> md5 {};
};

struct NativeFrameHeaderInfo {
    unsigned sync = 0;
    unsigned reserved_before_blocking_strategy = 0;
    unsigned blocking_strategy = 0;
    unsigned block_size_code = 0;
    unsigned sample_rate_code = 0;
    unsigned channel_assignment = 0;
    unsigned bits_per_sample_code = 0;
    unsigned reserved_after_bits_per_sample = 0;
    unsigned frame_number_first_byte = 0;
    unsigned block_size = 0;
    bool crc8_matches = false;
};

NativeStreamInfo native_streaminfo(const std::filesystem::path& flac_path)
{
    constexpr std::size_t kStreamInfoBytes = 42;
    const auto bytes = read_file(flac_path);
    require(bytes.size() >= kStreamInfoBytes, "native FLAC file was shorter than STREAMINFO");
    require(bytes.substr(0, 4) == "fLaC", "native FLAC marker mismatch");

    NativeStreamInfo info {
        .is_last_metadata_block = (byte_at(bytes, 4) & 0x80U) != 0,
        .metadata_type = byte_at(bytes, 4) & 0x7fU,
        .metadata_length = read_u24be(bytes, 5),
        .min_block_size = read_u16be(bytes, 8),
        .max_block_size = read_u16be(bytes, 10),
        .min_frame_size = read_u24be(bytes, 12),
        .max_frame_size = read_u24be(bytes, 15),
        .sample_rate = read_bits(bytes, 18U * 8U, 20),
        .channels = read_bits(bytes, (18U * 8U) + 20U, 3) + 1U,
        .bits_per_sample = read_bits(bytes, (18U * 8U) + 23U, 5) + 1U,
        .total_samples = read_bits64(bytes, (18U * 8U) + 28U, 36),
    };
    for (std::size_t i = 0; i < info.md5.size(); ++i) {
        info.md5[i] = byte_at(bytes, 26U + i);
    }
    return info;
}

NativeFrameHeaderInfo native_first_frame_header(const std::filesystem::path& flac_path)
{
    constexpr std::size_t kStreamInfoBytes = 42;
    constexpr std::size_t kFixedFrameHeaderBytes = 4;
    const auto bytes = read_file(flac_path);
    const auto frame_header_bit_offset = kStreamInfoBytes * 8U;
    const auto block_size_code = read_bits(bytes, frame_header_bit_offset + 16U, 4);
    const std::size_t frame_number_offset = kStreamInfoBytes + kFixedFrameHeaderBytes;
    const std::size_t block_size_extension_offset = frame_number_offset + 1U;
    const std::size_t block_size_extension_bytes = block_size_code == 6
        ? 1U
        : (block_size_code == 7 ? 2U : 0U);
    const auto crc8_offset = block_size_extension_offset + block_size_extension_bytes;

    unsigned block_size = 0;
    if (block_size_code == 6) {
        block_size = byte_at(bytes, block_size_extension_offset) + 1U;
    } else if (block_size_code == 7) {
        block_size = read_u16be(bytes, block_size_extension_offset) + 1U;
    }

    return NativeFrameHeaderInfo {
        .sync = read_bits(bytes, frame_header_bit_offset, 14),
        .reserved_before_blocking_strategy = read_bits(bytes, frame_header_bit_offset + 14U, 1),
        .blocking_strategy = read_bits(bytes, frame_header_bit_offset + 15U, 1),
        .block_size_code = block_size_code,
        .sample_rate_code = read_bits(bytes, frame_header_bit_offset + 20U, 4),
        .channel_assignment = read_bits(bytes, frame_header_bit_offset + 24U, 4),
        .bits_per_sample_code = read_bits(bytes, frame_header_bit_offset + 28U, 3),
        .reserved_after_bits_per_sample = read_bits(bytes, frame_header_bit_offset + 31U, 1),
        .frame_number_first_byte = byte_at(bytes, frame_number_offset),
        .block_size = block_size,
        .crc8_matches = ldcompress::flac_crc8(
            reinterpret_cast<const std::uint8_t*>(bytes.data() + kStreamInfoBytes),
            crc8_offset - kStreamInfoBytes) == byte_at(bytes, crc8_offset),
    };
}

std::size_t first_frame_subframe_bit_offset(std::string_view bytes)
{
    constexpr std::size_t kStreamInfoBytes = 42;
    constexpr std::size_t kFrameHeaderBytes = 4;
    constexpr std::size_t kFrameNumberBytes = 1;
    constexpr std::size_t kCrc8Bytes = 1;
    const auto frame_header_bit_offset = kStreamInfoBytes * 8U;
    const auto block_size_code = read_bits(bytes, frame_header_bit_offset + 16U, 4);
    const std::size_t block_size_extension_bytes = block_size_code == 6
        ? 1
        : (block_size_code == 7 ? 2 : 0);
    return (kStreamInfoBytes + kFrameHeaderBytes + kFrameNumberBytes +
        block_size_extension_bytes + kCrc8Bytes) * 8U;
}

unsigned first_frame_subframe_type(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    return read_bits(bytes, first_frame_subframe_bit_offset(bytes) + 1U, 6);
}

FirstFrameSubframeInfo first_frame_subframe_info(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    const auto subframe_bit_offset = first_frame_subframe_bit_offset(bytes);
    FirstFrameSubframeInfo info {
        .type = read_bits(bytes, subframe_bit_offset + 1U, 6),
        .wasted_bits = 0,
        .sample_data_bit_offset = subframe_bit_offset + 8U,
    };

    if (read_bits(bytes, subframe_bit_offset + 7U, 1) != 0) {
        unsigned leading_zeroes = 0;
        while (read_bits(bytes, info.sample_data_bit_offset + leading_zeroes, 1) == 0) {
            ++leading_zeroes;
        }
        info.wasted_bits = leading_zeroes + 1U;
        info.sample_data_bit_offset += leading_zeroes + 1U;
    }

    return info;
}

unsigned first_frame_wasted_bits(const std::filesystem::path& flac_path)
{
    return first_frame_subframe_info(flac_path).wasted_bits;
}

FirstFrameFixedInfo first_frame_fixed_info(const std::filesystem::path& flac_path)
{
    const auto bytes = read_file(flac_path);
    const auto subframe_info = first_frame_subframe_info(flac_path);
    if (subframe_info.type < 8 || subframe_info.type > 12) {
        throw std::runtime_error("first frame did not use a fixed predictor subframe");
    }
    const auto order = subframe_info.type - 8;
    const auto effective_bits_per_sample = 16U - subframe_info.wasted_bits;
    const auto partition_order_bit_offset =
        subframe_info.sample_data_bit_offset +
        (static_cast<std::size_t>(order) * effective_bits_per_sample) + 2U;
    return FirstFrameFixedInfo {
        .order = order,
        .partition_order = read_bits(bytes, partition_order_bit_offset, 4),
        .wasted_bits = subframe_info.wasted_bits,
    };
}

using FrameWriter = ldcompress::FlacSubframeDecision (*)(
    std::ostream&,
    const std::vector<std::int32_t>&,
    const ldcompress::FlacFrameInfo&);

ldcompress::FlacFrameInfo make_frame_info(
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 10,
    unsigned max_rice_partition_order = 4)
{
    return ldcompress::FlacFrameInfo {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = max_lpc_order,
        .lpc_coefficient_precision = lpc_coefficient_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
}

void require_same_decision(
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

ldcompress::FlacSubframeDecision write_fixed_rice_file(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    FrameWriter writer = ldcompress::write_mono_fixed_rice_frame,
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 10,
    unsigned max_rice_partition_order = 4)
{
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }

    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    const auto frame_info = make_frame_info(
        max_lpc_order, lpc_coefficient_precision, max_rice_partition_order);
    return writer(output, samples, frame_info);
}

ldcompress::FlacSubframeDecision write_profiled_best_file(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    ldcompress::NativeAnalysisProfile profile,
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5)
{
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }

    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    const auto frame_info = make_frame_info(
        max_lpc_order, lpc_coefficient_precision, max_rice_partition_order);
    return ldcompress::write_mono_best_frame(output, samples, frame_info, profile);
}

ldcompress::FlacSubframeDecision write_selected_file(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacSelectedSubframe& selected,
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5)
{
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }

    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    const auto frame_info = make_frame_info(
        max_lpc_order, lpc_coefficient_precision, max_rice_partition_order);
    return ldcompress::write_mono_selected_frame(output, samples, frame_info, selected);
}

ldcompress::FlacSubframeDecision write_selected_file_with_decision(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacSelectedSubframe& selected,
    const ldcompress::FlacSubframeDecision& decision,
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5)
{
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }

    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    const auto frame_info = make_frame_info(
        max_lpc_order, lpc_coefficient_precision, max_rice_partition_order);
    return ldcompress::write_mono_selected_frame_with_decision(
        output, std::span<const std::int32_t>(samples), frame_info, selected, decision);
}

void verify_best_analysis_matches_writer(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    const char* label,
    unsigned max_lpc_order = 12,
    unsigned lpc_coefficient_precision = 10,
    unsigned max_rice_partition_order = 4)
{
    const auto written = write_fixed_rice_file(
        flac_path, samples, ldcompress::write_mono_best_frame, max_lpc_order,
        lpc_coefficient_precision, max_rice_partition_order);
    const auto analyzed = ldcompress::analyze_mono_best_frame(samples, make_frame_info(
        max_lpc_order, lpc_coefficient_precision, max_rice_partition_order));
    require_same_decision(analyzed, written, label);

    const auto second_path = flac_path.parent_path() /
        (flac_path.filename().string() + ".second");
    const auto written_after_analysis = write_fixed_rice_file(
        second_path, samples, ldcompress::write_mono_best_frame, max_lpc_order,
        lpc_coefficient_precision, max_rice_partition_order);
    require_same_decision(written_after_analysis, written, label);
    require(read_file(second_path) == read_file(flac_path),
        "native best analysis changed subsequent writer output bytes");
}

void verify_fixed_rice_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned expected_order,
    unsigned expected_partition_order,
    int expected_wasted_bits = -1,
    unsigned max_rice_partition_order = 4)
{
    const auto decision = write_fixed_rice_file(
        flac_path, samples, ldcompress::write_mono_fixed_rice_frame, 12,
        12, max_rice_partition_order);
    require(decision.rice_partition_order == expected_partition_order,
        "native fixed/Rice writer reported an unexpected Rice partition order");
    const auto frame_info = first_frame_fixed_info(flac_path);
    require(frame_info.order == expected_order,
        "native fixed/Rice writer chose an unexpected fixed predictor order");
    require(frame_info.partition_order == expected_partition_order,
        "native fixed/Rice writer chose an unexpected Rice partition order");
    if (expected_wasted_bits >= 0) {
        require(frame_info.wasted_bits == static_cast<unsigned>(expected_wasted_bits),
            "native fixed/Rice writer chose an unexpected wasted-bits count");
    }

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected fixed/Rice decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected fixed/Rice decoded LDS byte count");
    require(decoded.str() == expected, "native fixed/Rice FLAC did not round-trip to expected LDS");
}

void verify_selected_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned expected_type,
    int expected_wasted_bits = -1)
{
    write_fixed_rice_file(flac_path, samples, ldcompress::write_mono_best_frame);
    require(first_frame_subframe_type(flac_path) == expected_type,
        "native best-frame writer chose an unexpected subframe type");
    if (expected_wasted_bits >= 0) {
        require(first_frame_wasted_bits(flac_path) == static_cast<unsigned>(expected_wasted_bits),
            "native best-frame writer chose an unexpected wasted-bits count");
    }

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected best-frame decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected best-frame decoded LDS byte count");
    require(decoded.str() == expected, "native best-frame FLAC did not round-trip to expected LDS");
}

void verify_lpc_round_trip(
    const std::filesystem::path& flac_path,
    const std::vector<std::int32_t>& samples,
    unsigned lpc_coefficient_precision = 10)
{
    const auto decision = write_fixed_rice_file(
        flac_path, samples, ldcompress::write_mono_best_frame, 12,
        lpc_coefficient_precision, 4);
    require(decision.kind == ldcompress::FlacSubframeKind::LpcRice,
        "native best-frame writer did not choose LPC for LPC-friendly samples");
    require(decision.lpc_order >= 1 && decision.lpc_order <= 12,
        "native best-frame writer chose an unexpected LPC order");
    const auto type = first_frame_subframe_type(flac_path);
    require(type >= 0x20 && type <= 0x2b,
        "native best-frame writer did not write an expected LPC subframe type");

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected LPC decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected LPC decoded LDS byte count");
    require(decoded.str() == expected, "native LPC FLAC did not round-trip to expected LDS");
}

bool is_predictive(const ldcompress::FlacSubframeDecision& decision)
{
    return decision.kind == ldcompress::FlacSubframeKind::FixedRice ||
        decision.kind == ldcompress::FlacSubframeKind::LpcRice;
}

void test_native_verbatim_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-writer-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "verbatim.flac";
    const auto samples = make_samples();

    {
        std::ofstream output(flac_path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not create test FLAC file");
        }

        const ldcompress::FlacStreamInfo stream_info {
            .min_block_size = static_cast<unsigned>(samples.size()),
            .max_block_size = static_cast<unsigned>(samples.size()),
            .min_frame_size = 0,
            .max_frame_size = 0,
            .sample_rate = 40000,
            .channels = 1,
            .bits_per_sample = 16,
            .total_samples = samples.size(),
            .md5 = md5_samples_s16le(samples),
        };
        ldcompress::write_native_flac_streaminfo(output, stream_info);

        const ldcompress::FlacFrameInfo frame_info {
            .frame_number = 0,
            .sample_rate = 40000,
            .bits_per_sample = 16,
        };
        ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    }

    require(ldcompress::detect_flac_container(flac_path.string()) == ldcompress::FlacContainer::Native,
        "native writer output was not detected as native FLAC");
    require(first_frame_wasted_bits(flac_path) == 6,
        "native verbatim writer did not mark LDS low zero bits as wasted");

    std::ostringstream decoded;
    ldcompress::FileDigest compressed_digest;
    const auto stats = ldcompress::decompress_flac_to_lds_with_input_digest(
        flac_path.string(), decoded, compressed_digest);
    const auto expected = pack_expected_lds(samples);

    require(stats.samples == samples.size(), "unexpected decoded sample count");
    require(stats.output_bytes == expected.size(), "unexpected decoded LDS byte count");
    require(decoded.str() == expected, "native verbatim FLAC did not round-trip to expected LDS");
    const auto expected_digest = ldcompress::md5_file(flac_path.string());
    require(compressed_digest.bytes == expected_digest.bytes,
        "native digest decode read an unexpected number of compressed bytes");
    require(compressed_digest.md5.digest() == expected_digest.md5.digest(),
        "native digest decode MD5 did not match md5_file");

    std::filesystem::remove_all(temp_dir);
}

void test_native_fixed_rice_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-fixed-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_fixed_rice_round_trip(temp_dir / "order0.flac", std::vector<std::int32_t>(16, 0), 0, 0);
    verify_fixed_rice_round_trip(temp_dir / "order1.flac", {
        -128, -64, -64, -128, 0, -128, -128, -256,
        -384, -512, -384, -512, -448, -512, -448, -576,
    }, 1, 0);
    verify_fixed_rice_round_trip(temp_dir / "order2.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    }, 2, 0, 6);

    std::vector<std::int32_t> quadratic;
    std::vector<std::int32_t> cubic;
    for (int i = 0; i < 16; ++i) {
        quadratic.push_back(i * i * 64);
    }
    for (int i = 0; i < 8; ++i) {
        cubic.push_back(i * i * i * 64);
    }
    verify_fixed_rice_round_trip(temp_dir / "order3.flac", quadratic, 3, 0);
    verify_fixed_rice_round_trip(temp_dir / "order4.flac", cubic, 4, 0);
    const std::vector<std::int32_t> partitioned {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    };
    verify_fixed_rice_round_trip(temp_dir / "partitioned.flac", partitioned, 0, 1);
    verify_fixed_rice_round_trip(temp_dir / "partitioned-max0.flac", partitioned, 0, 0, -1, 0);
    verify_fixed_rice_round_trip(temp_dir / "tiny.flac", {0, -64, 64, -128}, 0, 0);
    verify_fixed_rice_round_trip(temp_dir / "wide.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 0, 0);

    std::filesystem::remove_all(temp_dir);
}

void test_native_rice_partition_order_limit()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-rice-limit-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    bool threw = false;
    try {
        write_fixed_rice_file(temp_dir / "too-wide.flac",
            std::vector<std::int32_t>(16, 0),
            ldcompress::write_mono_fixed_rice_frame, 12, 12, 9);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "native writer accepted an out-of-range Rice partition order");

    std::filesystem::remove_all(temp_dir);
}

void test_native_selected_subframe_writer()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-selected-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const std::vector<std::int32_t> constant_samples(32, 2048);
    const ldcompress::FlacSelectedSubframe constant {
        .kind = ldcompress::FlacSubframeKind::Constant,
        .wasted_bits = 11,
    };
    const auto constant_decision =
        write_selected_file(temp_dir / "constant.flac", constant_samples, constant);
    require(constant_decision.kind == ldcompress::FlacSubframeKind::Constant,
        "selected constant writer returned the wrong decision kind");
    require(first_frame_subframe_type(temp_dir / "constant.flac") == 0,
        "selected constant writer wrote the wrong subframe type");

    const std::vector<std::int32_t> fixed_samples {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    };
    const ldcompress::FlacSelectedSubframe fixed {
        .kind = ldcompress::FlacSubframeKind::FixedRice,
        .fixed_order = 2,
        .rice_partition_order = 0,
        .wasted_bits = 6,
    };
    const auto fixed_decision =
        write_selected_file(temp_dir / "fixed.flac", fixed_samples, fixed);
    require(fixed_decision.kind == ldcompress::FlacSubframeKind::FixedRice,
        "selected fixed writer returned the wrong decision kind");
    require(fixed_decision.fixed_order == 2,
        "selected fixed writer returned the wrong predictor order");
    require(first_frame_subframe_type(temp_dir / "fixed.flac") == 10,
        "selected fixed writer wrote the wrong subframe type");

    auto fixed_with_rice_parameters = fixed;
    fixed_with_rice_parameters.rice_parameters = {0};
    const auto fixed_params_decision = write_selected_file(
        temp_dir / "fixed-params.flac", fixed_samples, fixed_with_rice_parameters);
    require_same_decision(fixed_params_decision, fixed_decision,
        "selected fixed writer with supplied Rice parameters");
    require(read_file(temp_dir / "fixed-params.flac") == read_file(temp_dir / "fixed.flac"),
        "selected fixed writer changed bytes with supplied Rice parameters");

    auto trusted_fixed_decision = fixed_params_decision;
    trusted_fixed_decision.estimated_bits += 13;
    const auto trusted_fixed_written = write_selected_file_with_decision(
        temp_dir / "fixed-trusted.flac",
        fixed_samples,
        fixed_with_rice_parameters,
        trusted_fixed_decision);
    require(trusted_fixed_written.estimated_bits == trusted_fixed_decision.estimated_bits,
        "trusted selected fixed writer did not return the supplied bit estimate");
    require(read_file(temp_dir / "fixed-trusted.flac") == read_file(temp_dir / "fixed-params.flac"),
        "trusted selected fixed writer changed emitted FLAC bytes");

    const std::vector<std::int32_t> zero_samples(16, 0);
    auto zero_fixed = fixed;
    zero_fixed.wasted_bits = 15;
    const auto zero_fixed_decision =
        write_selected_file(temp_dir / "fixed-zero.flac", zero_samples, zero_fixed);
    require(zero_fixed_decision.kind == ldcompress::FlacSubframeKind::FixedRice,
        "selected fixed writer rejected the all-zero wasted-bits cap");

    const auto lpc_samples = make_lpc_friendly_samples();
    const auto frame_info = make_frame_info(12, 12, 5);
    const auto lpc = ldcompress::analyze_mono_lpc_frame(lpc_samples, frame_info);
    require(lpc.has_value(), "native selected writer test could not prepare LPC selection");
    const ldcompress::FlacSelectedSubframe lpc_selected {
        .kind = ldcompress::FlacSubframeKind::LpcRice,
        .lpc_order = lpc->order,
        .rice_partition_order = lpc->rice_partition_order,
        .wasted_bits = lpc->wasted_bits,
        .coefficient_precision = lpc->coefficient_precision,
        .quantization_shift = lpc->quantization_shift,
        .coefficients = lpc->coefficients,
    };
    const auto lpc_decision =
        write_selected_file(temp_dir / "lpc.flac", lpc_samples, lpc_selected);
    require(lpc_decision.kind == ldcompress::FlacSubframeKind::LpcRice,
        "selected LPC writer returned the wrong decision kind");
    require(lpc_decision.lpc_order == lpc->order,
        "selected LPC writer returned the wrong predictor order");
    require(first_frame_subframe_type(temp_dir / "lpc.flac") == 0x20 + lpc->order - 1U,
        "selected LPC writer wrote the wrong subframe type");

    auto trusted_lpc_decision = lpc_decision;
    trusted_lpc_decision.estimated_bits += 17;
    const auto trusted_lpc_written = write_selected_file_with_decision(
        temp_dir / "lpc-trusted.flac", lpc_samples, lpc_selected, trusted_lpc_decision);
    require(trusted_lpc_written.estimated_bits == trusted_lpc_decision.estimated_bits,
        "trusted selected writer did not return the supplied bit estimate");
    require(read_file(temp_dir / "lpc-trusted.flac") == read_file(temp_dir / "lpc.flac"),
        "trusted selected writer changed emitted FLAC bytes");

    const ldcompress::FlacSelectedSubframe lpc_with_rice_parameters {
        .kind = ldcompress::FlacSubframeKind::LpcRice,
        .lpc_order = 1,
        .rice_partition_order = 0,
        .wasted_bits = 6,
        .coefficient_precision = 2,
        .quantization_shift = 0,
        .coefficients = {1},
        .rice_parameters = {0},
    };
    const auto lpc_params_decision = write_selected_file(
        temp_dir / "lpc-params.flac", fixed_samples, lpc_with_rice_parameters);
    auto trusted_lpc_params_decision = lpc_params_decision;
    trusted_lpc_params_decision.estimated_bits += 19;
    const auto trusted_lpc_params_written = write_selected_file_with_decision(
        temp_dir / "lpc-params-trusted.flac",
        fixed_samples,
        lpc_with_rice_parameters,
        trusted_lpc_params_decision);
    require(trusted_lpc_params_written.estimated_bits == trusted_lpc_params_decision.estimated_bits,
        "trusted selected LPC writer with supplied Rice parameters did not return the supplied bit estimate");
    require(read_file(temp_dir / "lpc-params-trusted.flac") == read_file(temp_dir / "lpc-params.flac"),
        "trusted selected LPC writer changed bytes with supplied Rice parameters");

    for (const auto& item : {
             std::pair { temp_dir / "constant.flac", constant_samples },
             std::pair { temp_dir / "fixed.flac", fixed_samples },
             std::pair { temp_dir / "fixed-params.flac", fixed_samples },
             std::pair { temp_dir / "fixed-trusted.flac", fixed_samples },
             std::pair { temp_dir / "fixed-zero.flac", zero_samples },
             std::pair { temp_dir / "lpc.flac", lpc_samples },
             std::pair { temp_dir / "lpc-trusted.flac", lpc_samples },
             std::pair { temp_dir / "lpc-params.flac", fixed_samples },
             std::pair { temp_dir / "lpc-params-trusted.flac", fixed_samples },
         }) {
        std::ostringstream decoded;
        const auto stats = ldcompress::decompress_flac_to_lds(item.first.string(), decoded);
        const auto expected = pack_expected_lds(item.second);
        require(stats.samples == item.second.size(),
            "selected writer decoded sample count mismatch");
        require(decoded.str() == expected,
            "selected writer FLAC did not round-trip to expected LDS");
    }

    const auto expect_selected_write_throws =
        [&](const char* path, const std::vector<std::int32_t>& samples,
            const ldcompress::FlacSelectedSubframe& selected, const char* message) {
            bool threw = false;
            try {
                (void)write_selected_file(temp_dir / path, samples, selected);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            require(threw, message);
        };
    const auto expect_trusted_selected_write_throws =
        [&](const char* path, const std::vector<std::int32_t>& samples,
            const ldcompress::FlacSelectedSubframe& selected, const char* message) {
            ldcompress::FlacSubframeDecision decision {
                .kind = selected.kind,
                .fixed_order = selected.fixed_order,
                .lpc_order = selected.lpc_order,
                .rice_partition_order = selected.rice_partition_order,
                .wasted_bits = selected.wasted_bits,
                .estimated_bits = 1,
            };
            bool threw = false;
            try {
                (void)write_selected_file_with_decision(
                    temp_dir / path, samples, selected, decision);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            require(threw, message);
        };

    auto bad_wasted_bits = fixed;
    bad_wasted_bits.wasted_bits = 0;
    expect_selected_write_throws("bad-wbits.flac", fixed_samples, bad_wasted_bits,
        "selected writer accepted a too-low wasted-bits count");
    expect_trusted_selected_write_throws("bad-trusted-wbits.flac", fixed_samples,
        bad_wasted_bits, "trusted selected writer accepted a too-low wasted-bits count");

    bad_wasted_bits.wasted_bits = 7;
    expect_selected_write_throws("bad-high-wbits.flac", fixed_samples, bad_wasted_bits,
        "selected writer accepted a too-high wasted-bits count");
    expect_trusted_selected_write_throws("bad-trusted-high-wbits.flac", fixed_samples,
        bad_wasted_bits, "trusted selected writer accepted a too-high wasted-bits count");

    bad_wasted_bits = zero_fixed;
    bad_wasted_bits.wasted_bits = 16;
    expect_selected_write_throws("bad-cap-wbits.flac", zero_samples, bad_wasted_bits,
        "selected writer accepted a wasted-bits count at the bit depth");

    auto invalid_late_sample = fixed;
    invalid_late_sample.fixed_order = 0;
    invalid_late_sample.wasted_bits = 0;
    auto invalid_late_samples = fixed_samples;
    invalid_late_samples.front() = 1;
    invalid_late_samples[1] = 40000;
    expect_selected_write_throws("bad-late-sample.flac", invalid_late_samples,
        invalid_late_sample, "selected writer stopped validating samples after zero wasted bits");
    expect_trusted_selected_write_throws("bad-trusted-late-sample.flac",
        invalid_late_samples, invalid_late_sample,
        "trusted selected writer stopped validating samples after zero wasted bits");

    auto bad_rice_parameters = fixed_with_rice_parameters;
    bad_rice_parameters.rice_parameters = {0, 0};
    bool threw = false;
    try {
        (void)write_selected_file(
            temp_dir / "bad-rice-params.flac", fixed_samples, bad_rice_parameters);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "selected writer accepted a mismatched Rice parameter count");

    auto bad_quantization_shift = lpc_selected;
    bad_quantization_shift.quantization_shift = -1;
    threw = false;
    try {
        (void)write_selected_file(
            temp_dir / "bad-quantization-shift.flac", lpc_samples, bad_quantization_shift);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "selected writer accepted a negative LPC quantization shift");

    std::filesystem::remove_all(temp_dir);
}

void test_native_lpc_precision_limit()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-lpc-precision-limit-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    for (const unsigned precision : {0U, 16U}) {
        bool threw = false;
        try {
            write_fixed_rice_file(temp_dir / (std::string("bad-precision-") +
                    std::to_string(precision) + ".flac"),
                make_lpc_friendly_samples(), ldcompress::write_mono_best_frame, 12, precision, 4);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "native writer accepted an out-of-range LPC coefficient precision");
    }

    std::filesystem::remove_all(temp_dir);
}

void test_native_best_subframe_selection()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-best-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_selected_round_trip(temp_dir / "constant.flac",
        std::vector<std::int32_t>(16, -4096), 0, 12);
    verify_selected_round_trip(temp_dir / "fixed.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    }, 10, 6);
    verify_selected_round_trip(temp_dir / "verbatim.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    }, 1, 6);
    verify_lpc_round_trip(temp_dir / "lpc.flac", make_lpc_friendly_samples());
    verify_lpc_round_trip(temp_dir / "lpc-precision12.flac",
        make_lpc_friendly_samples(), 12);
    verify_lpc_round_trip(temp_dir / "lpc-precision15.flac",
        make_lpc_friendly_samples(), 15);

    std::filesystem::remove_all(temp_dir);
}

void test_native_subframe_analysis_matches_writer()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-analysis-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_best_analysis_matches_writer(temp_dir / "constant.flac",
        std::vector<std::int32_t>(16, -4096),
        "native best/constant analysis did not match writer decision");
    verify_best_analysis_matches_writer(temp_dir / "fixed.flac", {
        0, 64, 128, 192, 256, 320, 384, 448,
        512, 576, 640, 704, 768, 832, 896, 960,
    },
        "native best/fixed analysis did not match writer decision");
    verify_best_analysis_matches_writer(temp_dir / "small.flac",
        make_samples(),
        "native best/small analysis did not match writer decision");
    verify_best_analysis_matches_writer(temp_dir / "best-verbatim.flac", {
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    },
        "native best/verbatim analysis did not match writer decision");
    verify_best_analysis_matches_writer(temp_dir / "best-lpc.flac",
        make_lpc_friendly_samples(),
        "native best/LPC analysis did not match writer decision",
        12, 12, 5);

    std::filesystem::remove_all(temp_dir);
}

void test_native_lpc_analysis_surface()
{
    const auto samples = make_lpc_friendly_samples();
    const auto frame_info = make_frame_info(12, 12, 5);
    const auto lpc = ldcompress::analyze_mono_lpc_frame(samples, frame_info);
    require(lpc.has_value(), "native LPC analysis did not return an LPC candidate");

    const auto decision = ldcompress::analyze_mono_best_frame(samples, frame_info);
    require(decision.kind == ldcompress::FlacSubframeKind::LpcRice,
        "native best analysis did not choose LPC for LPC-friendly samples");
    require(lpc->order == decision.lpc_order,
        "native LPC analysis order did not match best-frame decision");
    require(lpc->rice_partition_order == decision.rice_partition_order,
        "native LPC analysis partition order did not match best-frame decision");
    require(lpc->wasted_bits == decision.wasted_bits,
        "native LPC analysis wasted bits did not match best-frame decision");
    require(lpc->estimated_bits == decision.estimated_bits,
        "native LPC analysis bit count did not match best-frame decision");
    require(lpc->coefficient_precision == frame_info.lpc_coefficient_precision,
        "native LPC analysis coefficient precision mismatch");
    require(lpc->quantization_shift >= 0 && lpc->quantization_shift <= 15,
        "native LPC analysis quantization shift out of range");
    require(lpc->coefficients.size() == lpc->order,
        "native LPC analysis coefficient count mismatch");
    require(std::any_of(lpc->coefficients.begin(), lpc->coefficients.end(), [](std::int32_t value) {
        return value != 0;
    }), "native LPC analysis returned all-zero coefficients");

    const auto per_order = ldcompress::analyze_mono_lpc_order(samples, frame_info, lpc->order);
    require(per_order.has_value(), "native per-order LPC analysis did not return the winning order");
    require(per_order->order == lpc->order,
        "native per-order LPC analysis returned an unexpected order");
    require(per_order->rice_partition_order == lpc->rice_partition_order,
        "native per-order LPC analysis partition order mismatch");
    require(per_order->wasted_bits == lpc->wasted_bits,
        "native per-order LPC analysis wasted-bits mismatch");
    require(per_order->coefficient_precision == lpc->coefficient_precision,
        "native per-order LPC analysis coefficient precision mismatch");
    require(per_order->quantization_shift == lpc->quantization_shift,
        "native per-order LPC analysis quantization shift mismatch");
    require(per_order->coefficients == lpc->coefficients,
        "native per-order LPC analysis coefficient vector mismatch");
    require(per_order->estimated_bits == lpc->estimated_bits,
        "native per-order LPC analysis bit count mismatch");

    auto disabled_info = frame_info;
    disabled_info.max_lpc_order = 0;
    require(!ldcompress::analyze_mono_lpc_frame(samples, disabled_info).has_value(),
        "native LPC analysis returned a candidate when LPC order was disabled");
    require(!ldcompress::analyze_mono_lpc_order(samples, disabled_info, 1).has_value(),
        "native per-order LPC analysis returned a candidate when LPC order was disabled");
    require(!ldcompress::analyze_mono_lpc_frame(make_samples(), frame_info).has_value(),
        "native LPC analysis returned a candidate for a too-small block");
}

void test_native_mean_estimate_profiles()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-profile-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto samples = make_lpc_friendly_samples();
    const auto order_mean = write_profiled_best_file(
        temp_dir / "order-mean.flac",
        samples,
        ldcompress::NativeAnalysisProfile::OrderGuessMeanRice);
    const auto order_estimate = write_profiled_best_file(
        temp_dir / "order-estimate.flac",
        samples,
        ldcompress::NativeAnalysisProfile::OrderGuessMeanEstimateRice);
    const auto subdivide_mean = write_profiled_best_file(
        temp_dir / "subdivide-mean.flac",
        samples,
        ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanRice);
    const auto subdivide_estimate = write_profiled_best_file(
        temp_dir / "subdivide-estimate.flac",
        samples,
        ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice);

    require(is_predictive(order_mean),
        "native order-guess mean Rice profile did not choose a predictive subframe");
    require(is_predictive(order_estimate),
        "native order-guess mean-estimate Rice profile did not choose a predictive subframe");
    require(is_predictive(subdivide_mean),
        "native subdivide mean Rice profile did not choose a predictive subframe");
    require(is_predictive(subdivide_estimate),
        "native subdivide mean-estimate Rice profile did not choose a predictive subframe");

    require(order_mean.estimated_bits != order_estimate.estimated_bits,
        "native order-guess mean-estimate profile still returned exact-recost bits");
    require(subdivide_mean.estimated_bits != subdivide_estimate.estimated_bits,
        "native subdivide mean-estimate profile still returned exact-recost bits");

    const auto expected = pack_expected_lds(samples);
    for (const auto path : {
             temp_dir / "order-mean.flac",
             temp_dir / "order-estimate.flac",
             temp_dir / "subdivide-mean.flac",
             temp_dir / "subdivide-estimate.flac",
         }) {
        std::ostringstream decoded;
        const auto stats = ldcompress::decompress_flac_to_lds(path.string(), decoded);
        require(stats.samples == samples.size(),
            "native profiled writer decoded sample count mismatch");
        require(stats.output_bytes == expected.size(),
            "native profiled writer decoded LDS byte count mismatch");
        require(decoded.str() == expected,
            "native profiled writer FLAC did not round-trip to expected LDS");
    }

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_and_frame_header_contract()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-contract-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "contract.flac";
    const auto samples = make_lpc_friendly_samples();
    write_fixed_rice_file(flac_path, samples, ldcompress::write_mono_best_frame, 12, 12, 5);

    const auto streaminfo = native_streaminfo(flac_path);
    require(streaminfo.is_last_metadata_block, "native STREAMINFO was not marked as the last metadata block");
    require(streaminfo.metadata_type == 0, "native STREAMINFO metadata type was not STREAMINFO");
    require(streaminfo.metadata_length == 34, "native STREAMINFO metadata length was not 34 bytes");
    require(streaminfo.min_block_size == samples.size(), "native STREAMINFO min block size mismatch");
    require(streaminfo.max_block_size == samples.size(), "native STREAMINFO max block size mismatch");
    require(streaminfo.min_frame_size == 0, "native STREAMINFO min frame size should be unknown");
    require(streaminfo.max_frame_size == 0, "native STREAMINFO max frame size should be unknown");
    require(streaminfo.sample_rate == 40000, "native STREAMINFO sample rate mismatch");
    require(streaminfo.channels == 1, "native STREAMINFO channel count mismatch");
    require(streaminfo.bits_per_sample == 16, "native STREAMINFO bits-per-sample mismatch");
    require(streaminfo.total_samples == samples.size(), "native STREAMINFO total sample count mismatch");
    require(streaminfo.md5 == md5_samples_s16le(samples), "native STREAMINFO sample MD5 mismatch");

    const auto frame = native_first_frame_header(flac_path);
    require(frame.sync == 0x3ffe, "native FLAC frame sync mismatch");
    require(frame.reserved_before_blocking_strategy == 0, "native FLAC reserved frame bit was set");
    require(frame.blocking_strategy == 0, "native FLAC frame did not use fixed-block numbering");
    require(frame.block_size_code == 7, "native FLAC frame did not use 16-bit block-size extension");
    require(frame.sample_rate_code == 0, "native FLAC frame should use STREAMINFO sample rate");
    require(frame.channel_assignment == 0, "native FLAC frame channel assignment was not mono");
    require(frame.bits_per_sample_code == 4, "native FLAC frame bits-per-sample code was not 16-bit");
    require(frame.reserved_after_bits_per_sample == 0, "native FLAC trailing reserved frame bit was set");
    require(frame.frame_number_first_byte == 0, "native FLAC first frame number was not zero");
    require(frame.block_size == samples.size(), "native FLAC frame block-size extension mismatch");
    require(frame.crc8_matches, "native FLAC frame header CRC-8 mismatch");

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_large_total_samples_are_unknown()
{
    constexpr std::uint64_t kMaxEncodedTotalSamples = (1ULL << 36U) - 1U;
    constexpr std::uint64_t kSamplesAt80GiBLds = 1ULL << 36U;

    require(ldcompress::flac_streaminfo_total_samples_or_unknown(0) == 0,
        "empty native STREAMINFO sample count was not unknown");
    require(ldcompress::flac_streaminfo_total_samples_or_unknown(kMaxEncodedTotalSamples) ==
            kMaxEncodedTotalSamples,
        "largest encodable native STREAMINFO sample count changed");
    require(ldcompress::flac_streaminfo_total_samples_or_unknown(kSamplesAt80GiBLds) == 0,
        "80 GiB LDS sample count was not converted to unknown STREAMINFO length");
    require(ldcompress::flac_streaminfo_total_samples_or_unknown(UINT64_MAX) == 0,
        "oversized native STREAMINFO sample count was not converted to unknown");

    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-large-streaminfo-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "large-count.flac";
    const auto samples = make_samples();
    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create large-count test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = kSamplesAt80GiBLds,
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    output.close();

    const auto streaminfo = native_streaminfo(flac_path);
    require(streaminfo.total_samples == 0,
        "large-count native STREAMINFO did not serialize an unknown sample count");

    std::ostringstream decoded;
    const auto decode_stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    require(decode_stats.samples == samples.size(),
        "unknown-count native FLAC decoded sample count mismatch");
    require(decoded.str() == pack_expected_lds(samples),
        "unknown-count native FLAC did not round-trip to expected LDS");

    std::filesystem::remove_all(temp_dir);
}

std::vector<std::int32_t> make_counted_samples(std::size_t count)
{
    std::vector<std::int32_t> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = ((static_cast<int>(i * 37U) % 1024) - 512) * 64;
        samples.push_back(value);
    }
    return samples;
}

void verify_native_encoder_streaminfo_block_bounds(
    const std::filesystem::path& flac_path,
    std::size_t sample_count,
    unsigned expected_streaminfo_block_size)
{
    constexpr unsigned kFrameSamples = 16;
    const auto samples = make_counted_samples(sample_count);
    const auto packed = pack_expected_lds(samples);
    std::istringstream input(packed);

    const auto encode_stats = ldcompress::compress_lds_to_native_verbatim_flac(
        input,
        flac_path.string(),
        40000,
        1,
        kFrameSamples,
        0,
        12,
        0);
    require(encode_stats.samples == sample_count, "short-tail native encode sample count mismatch");

    const auto streaminfo = native_streaminfo(flac_path);
    require(streaminfo.min_block_size == expected_streaminfo_block_size,
        "short-tail native STREAMINFO min block size mismatch");
    require(streaminfo.max_block_size == expected_streaminfo_block_size,
        "short-tail native STREAMINFO max block size mismatch");
    require(streaminfo.total_samples == sample_count,
        "short-tail native STREAMINFO total sample count mismatch");

    std::ostringstream decoded;
    const auto decode_stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    require(decode_stats.samples == sample_count, "short-tail native decoded sample count mismatch");
    require(decoded.str() == packed, "short-tail native FLAC did not round-trip to expected LDS");
}

void test_native_encoder_streaminfo_block_bounds_for_short_tail()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-short-tail-streaminfo-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    verify_native_encoder_streaminfo_block_bounds(temp_dir / "tiny.flac", 4, 16);
    verify_native_encoder_streaminfo_block_bounds(temp_dir / "short-tail.flac", 20, 16);

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_md5_mismatch_is_reported()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-md5-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "bad-md5.flac";
    const auto samples = make_samples();
    auto md5 = md5_samples_s16le(samples);
    md5[0] ^= 0xffU;

    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5,
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    output.close();

    std::ostringstream decoded;
    const auto stats = ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    require(stats.streaminfo_pcm_md5_mismatch,
        "native FLAC decode did not report a bad STREAMINFO sample MD5");
    require(decoded.str() == pack_expected_lds(samples),
        "native FLAC MD5 mismatch changed decoded LDS bytes");

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_underreported_total_recovers_all_frames()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-underreported-total-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "underreported-total.flac";
    const auto first_frame_samples = make_samples();
    auto second_frame_samples = make_samples();
    for (auto& sample : second_frame_samples) {
        sample += 64;
    }
    auto all_samples = first_frame_samples;
    all_samples.insert(all_samples.end(), second_frame_samples.begin(), second_frame_samples.end());

    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(first_frame_samples.size()),
        .max_block_size = static_cast<unsigned>(first_frame_samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = first_frame_samples.size(),
        .md5 = md5_samples_s16le(all_samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);

    auto first_frame_info = make_frame_info();
    ldcompress::write_mono_verbatim_frame(output, first_frame_samples, first_frame_info);
    auto second_frame_info = first_frame_info;
    second_frame_info.frame_number = 1;
    ldcompress::write_mono_verbatim_frame(output, second_frame_samples, second_frame_info);
    output.close();

    std::ostringstream decoded;
    ldcompress::FileDigest compressed_digest;
    const auto stats = ldcompress::decompress_flac_to_lds_with_input_digest(
        flac_path.string(), decoded, compressed_digest);
    const auto expected_digest = ldcompress::md5_file(flac_path.string());
    require(stats.samples == all_samples.size(),
        "underreported STREAMINFO did not decode every valid frame");
    require(stats.streaminfo_declared_total_samples == first_frame_samples.size(),
        "underreported STREAMINFO declared total was not retained in decode stats");
    require(stats.streaminfo_total_samples_underreported,
        "underreported STREAMINFO total was not reported");
    require(decoded.str() == pack_expected_lds(all_samples),
        "underreported STREAMINFO changed recovered LDS bytes");
    require(compressed_digest.bytes == expected_digest.bytes,
        "underreported STREAMINFO digest did not cover complete input");
    require(compressed_digest.md5.digest() == expected_digest.md5.digest(),
        "underreported STREAMINFO digest did not match compressed input");

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_total_samples_shorter_than_stream_is_rejected()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-sample-count-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "bad-total-samples.flac";
    const auto samples = make_samples();

    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size() + 4U,
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    output.close();

    bool threw = false;
    try {
        std::ostringstream decoded;
        (void)ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "native FLAC decode accepted a stream shorter than STREAMINFO");

    std::filesystem::remove_all(temp_dir);
}

void test_native_streaminfo_non_lds_sample_count_is_rejected_before_writing()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-non-lds-sample-count-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "bad-lds-shape.flac";
    const auto samples = make_samples();

    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size() + 1U,
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    output.close();

    bool threw = false;
    std::ostringstream decoded;
    try {
        (void)ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "native FLAC decode accepted a non-LDS STREAMINFO sample count");
    require(decoded.str().empty(), "non-LDS STREAMINFO sample count wrote partial LDS output");

    std::filesystem::remove_all(temp_dir);
}

void test_native_off_grid_sample_is_rejected_before_writing()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-native-off-grid-sample-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);

    const auto flac_path = temp_dir / "off-grid.flac";
    auto samples = make_samples();
    samples.at(0) += 1;

    std::ofstream output(flac_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not create test FLAC file");
    }
    const ldcompress::FlacStreamInfo stream_info {
        .min_block_size = static_cast<unsigned>(samples.size()),
        .max_block_size = static_cast<unsigned>(samples.size()),
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = 40000,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = samples.size(),
        .md5 = md5_samples_s16le(samples),
    };
    ldcompress::write_native_flac_streaminfo(output, stream_info);
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
    };
    ldcompress::write_mono_verbatim_frame(output, samples, frame_info);
    output.close();

    std::ostringstream decoded;
    std::string error;
    try {
        (void)ldcompress::decompress_flac_to_lds(flac_path.string(), decoded);
    } catch (const std::runtime_error& exception) {
        error = exception.what();
    }
    require(!error.empty(), "native FLAC decode accepted an off-grid PCM sample");
    require(error.find("LDS 10-bit grid") != std::string::npos,
        "off-grid PCM sample reported the wrong decode error");
    require(decoded.str().empty(), "off-grid PCM sample wrote LDS bytes");

    std::filesystem::remove_all(temp_dir);
}

void test_native_frame_bit_depth_is_validated_before_sample_work()
{
    const auto samples = make_samples();
    const ldcompress::FlacFrameInfo invalid_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 0,
    };

    bool analyzer_threw = false;
    try {
        (void)ldcompress::analyze_mono_best_frame(samples, invalid_info);
    } catch (const std::runtime_error& exception) {
        analyzer_threw = std::string_view(exception.what()).find(
            "unsupported FLAC frame bits per sample") != std::string_view::npos;
    }
    require(analyzer_threw,
        "native frame analyzer accepted an invalid bit depth before sample validation");

    bool writer_threw = false;
    std::ostringstream output;
    try {
        (void)ldcompress::write_mono_verbatim_frame(output, samples, invalid_info);
    } catch (const std::runtime_error& exception) {
        writer_threw = std::string_view(exception.what()).find(
            "unsupported FLAC frame bits per sample") != std::string_view::npos;
    }
    require(writer_threw,
        "native frame writer accepted an invalid bit depth before sample validation");
}

}  // namespace

int main()
{
    try {
        test_native_verbatim_round_trip();
        test_native_fixed_rice_round_trip();
        test_native_rice_partition_order_limit();
        test_native_selected_subframe_writer();
        test_native_lpc_precision_limit();
        test_native_best_subframe_selection();
        test_native_subframe_analysis_matches_writer();
        test_native_lpc_analysis_surface();
        test_native_mean_estimate_profiles();
        test_native_streaminfo_and_frame_header_contract();
        test_native_streaminfo_large_total_samples_are_unknown();
        test_native_encoder_streaminfo_block_bounds_for_short_tail();
        test_native_streaminfo_md5_mismatch_is_reported();
        test_native_streaminfo_underreported_total_recovers_all_frames();
        test_native_streaminfo_total_samples_shorter_than_stream_is_rejected();
        test_native_streaminfo_non_lds_sample_count_is_rejected_before_writing();
        test_native_off_grid_sample_is_rejected_before_writing();
        test_native_frame_bit_depth_is_validated_before_sample_work();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_native_writer: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
