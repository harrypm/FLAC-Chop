#include "lds_codec.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string bytes(std::initializer_list<unsigned int> values)
{
    std::string result;
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void test_fixed_vectors()
{
    const ldcompress::SampleGroup zero_samples {0, 0, 0, 0};
    const ldcompress::PackedLdsGroup zero_packed {0x80, 0x20, 0x08, 0x02, 0x00};
    require(ldcompress::pack_group(zero_samples) == zero_packed,
        "zero samples did not pack to expected bytes");
    require(ldcompress::unpack_group(zero_packed) == zero_samples,
        "zero packed bytes did not unpack to expected samples");

    const ldcompress::SampleGroup edge_samples {-32768, -64, 0, 32704};
    const ldcompress::PackedLdsGroup edge_packed {0x00, 0x1f, 0xf8, 0x03, 0xff};
    require(ldcompress::pack_group(edge_samples) == edge_packed,
        "edge samples did not pack to expected bytes");
    require(ldcompress::unpack_group(edge_packed) == edge_samples,
        "edge packed bytes did not unpack to expected samples");

    const ldcompress::SampleGroup low_samples {-32768, -32768, -32768, -32768};
    const ldcompress::PackedLdsGroup low_packed {0x00, 0x00, 0x00, 0x00, 0x00};
    require(ldcompress::pack_group(low_samples) == low_packed,
        "minimum samples did not pack to expected bytes");
    require(ldcompress::unpack_group(low_packed) == low_samples,
        "minimum packed bytes did not unpack to expected samples");

    const ldcompress::SampleGroup high_samples {32704, 32704, 32704, 32704};
    const ldcompress::PackedLdsGroup high_packed {0xff, 0xff, 0xff, 0xff, 0xff};
    require(ldcompress::pack_group(high_samples) == high_packed,
        "maximum quantized samples did not pack to expected bytes");
    require(ldcompress::unpack_group(high_packed) == high_samples,
        "maximum packed bytes did not unpack to expected samples");

    const ldcompress::SampleGroup signed_division_samples {-1, -63, -64, -65};
    const ldcompress::PackedLdsGroup signed_division_packed {0x80, 0x20, 0x07, 0xfd, 0xff};
    require(ldcompress::pack_group(signed_division_samples) == signed_division_packed,
        "negative samples did not use C++ signed division semantics");
}

void test_stream_round_trip()
{
    std::string packed;
    for (int word = 0; word < 1024; word += 4) {
        const ldcompress::SampleGroup samples {
            static_cast<std::int16_t>((word - 512) * 64),
            static_cast<std::int16_t>((word + 1 - 512) * 64),
            static_cast<std::int16_t>((word + 2 - 512) * 64),
            static_cast<std::int16_t>((word + 3 - 512) * 64),
        };
        const auto group = ldcompress::pack_group(samples);
        packed.append(reinterpret_cast<const char*>(group.data()), group.size());
    }

    std::stringstream packed_input(packed);
    std::stringstream pcm;
    const auto unpack_stats = ldcompress::unpack_lds10_to_s16le(packed_input, pcm);
    require(unpack_stats.input_bytes == packed.size(), "wrong unpack input byte count");
    require(unpack_stats.output_bytes == 2048, "wrong unpack output byte count");
    require(unpack_stats.samples == 1024, "wrong unpack sample count");

    std::stringstream pcm_input(pcm.str());
    std::stringstream repacked;
    const auto pack_stats = ldcompress::pack_s16le_to_lds10(pcm_input, repacked);
    require(pack_stats.input_bytes == 2048, "wrong pack input byte count");
    require(pack_stats.output_bytes == packed.size(), "wrong pack output byte count");
    require(pack_stats.samples == 1024, "wrong pack sample count");
    require(repacked.str() == packed, "pack/unpack round trip changed bytes");
}

void test_truncated_inputs()
{
    bool saw_unpack_error = false;
    try {
        std::stringstream input(bytes({0x80, 0x20, 0x08, 0x02}));
        std::stringstream output;
        (void)ldcompress::unpack_lds10_to_s16le(input, output);
    } catch (const std::runtime_error&) {
        saw_unpack_error = true;
    }
    require(saw_unpack_error, "truncated LDS input did not fail");

    bool saw_pack_error = false;
    try {
        std::stringstream input(bytes({0x00, 0x00}));
        std::stringstream output;
        (void)ldcompress::pack_s16le_to_lds10(input, output);
    } catch (const std::runtime_error&) {
        saw_pack_error = true;
    }
    require(saw_pack_error, "truncated PCM input did not fail");
}

}  // namespace

int main()
{
    try {
        test_fixed_vectors();
        test_stream_round_trip();
        test_truncated_inputs();
    } catch (const std::exception& ex) {
        std::cerr << "test_lds_codec: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
