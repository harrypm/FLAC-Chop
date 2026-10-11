#include "lds_codec.h"

#include <array>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;

void write_all(std::ostream& output, const char* data, std::size_t size)
{
    output.write(data, static_cast<std::streamsize>(size));
    if (!output) {
        throw std::runtime_error("failed to write output");
    }
}

std::int16_t read_i16le(const std::uint8_t* bytes)
{
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8));
    return static_cast<std::int16_t>(value);
}

void write_i16le(std::uint8_t* bytes, std::int16_t sample)
{
    const auto value = static_cast<std::uint16_t>(sample);
    bytes[0] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

std::uint16_t sample_to_word(std::int16_t sample)
{
    return static_cast<std::uint16_t>((static_cast<int>(sample) / 64) + 512);
}

}  // namespace

SampleGroup unpack_group(PackedLdsGroup bytes)
{
    const int byte0 = bytes[0];
    const int byte1 = bytes[1];
    const int byte2 = bytes[2];
    const int byte3 = bytes[3];
    const int byte4 = bytes[4];

    const int word0 = ((byte0 & 0xff) * 4) + ((byte1 & 0xc0) >> 6);
    const int word1 = ((byte1 & 0x3f) * 16) + ((byte2 & 0xf0) >> 4);
    const int word2 = ((byte2 & 0x0f) * 64) + ((byte3 & 0xfc) >> 2);
    const int word3 = ((byte3 & 0x03) * 256) + (byte4 & 0xff);

    return {
        static_cast<std::int16_t>((word0 - 512) * 64),
        static_cast<std::int16_t>((word1 - 512) * 64),
        static_cast<std::int16_t>((word2 - 512) * 64),
        static_cast<std::int16_t>((word3 - 512) * 64),
    };
}

PackedLdsGroup pack_group(SampleGroup samples)
{
    const int word0 = sample_to_word(samples[0]);
    const int word1 = sample_to_word(samples[1]);
    const int word2 = sample_to_word(samples[2]);
    const int word3 = sample_to_word(samples[3]);

    return {
        static_cast<std::uint8_t>((word0 & 0x03fc) >> 2),
        static_cast<std::uint8_t>(((word0 & 0x0003) << 6) + ((word1 & 0x03f0) >> 4)),
        static_cast<std::uint8_t>(((word1 & 0x000f) << 4) + ((word2 & 0x03c0) >> 6)),
        static_cast<std::uint8_t>(((word2 & 0x003f) << 2) + ((word3 & 0x0300) >> 8)),
        static_cast<std::uint8_t>(word3 & 0x00ff),
    };
}

ConversionStats unpack_lds10_to_s16le(std::istream& input, std::ostream& output)
{
    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);
    std::vector<std::uint8_t> output_buffer(8 * kGroupsPerChunk);

    while (input) {
        input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        for (std::size_t i = 0; i < groups; ++i) {
            PackedLdsGroup packed;
            std::memcpy(packed.data(), input_buffer.data() + (i * 5), packed.size());
            const SampleGroup samples = unpack_group(packed);
            for (std::size_t sample = 0; sample < samples.size(); ++sample) {
                write_i16le(output_buffer.data() + (i * 8) + (sample * 2), samples[sample]);
            }
        }

        const auto output_bytes = groups * 8;
        write_all(output, reinterpret_cast<const char*>(output_buffer.data()), output_bytes);
        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.output_bytes += output_bytes;
        stats.samples += groups * 4;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    return stats;
}

ConversionStats pack_s16le_to_lds10(std::istream& input, std::ostream& output)
{
    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(8 * kGroupsPerChunk);
    std::vector<std::uint8_t> output_buffer(5 * kGroupsPerChunk);

    while (input) {
        input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 8) != 0) {
            throw std::runtime_error("truncated PCM input: byte count is not divisible by 8");
        }

        const auto groups = static_cast<std::size_t>(got / 8);
        for (std::size_t i = 0; i < groups; ++i) {
            SampleGroup samples;
            for (std::size_t sample = 0; sample < samples.size(); ++sample) {
                samples[sample] = read_i16le(input_buffer.data() + (i * 8) + (sample * 2));
            }
            const PackedLdsGroup packed = pack_group(samples);
            std::memcpy(output_buffer.data() + (i * 5), packed.data(), packed.size());
        }

        const auto output_bytes = groups * 5;
        write_all(output, reinterpret_cast<const char*>(output_buffer.data()), output_bytes);
        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.output_bytes += output_bytes;
        stats.samples += groups * 4;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read PCM input");
    }

    return stats;
}

}  // namespace ldcompress
