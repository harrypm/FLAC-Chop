#include "flac_codec.h"
#include "hash.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string make_lds_fixture()
{
    std::string data;
    for (int group = 0; group < 512; ++group) {
        const ldcompress::SampleGroup samples {
            static_cast<std::int16_t>((((group * 4) + 0) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 1) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 2) % 1024 - 512) * 64),
            static_cast<std::int16_t>((((group * 4) + 3) % 1024 - 512) * 64),
        };
        const auto packed = ldcompress::pack_group(samples);
        data.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    }
    return data;
}

void test_ogg_flac_round_trip()
{
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(temp_dir);
    const auto output_path = temp_dir / "fixture.ldf";

    const std::string fixture = make_lds_fixture();
    std::stringstream input(fixture);
    const auto encode_stats = ldcompress::compress_lds_to_flac(
        input, output_path.string(), ldcompress::FlacEncodeOptions {});
    require(encode_stats.input_bytes == fixture.size(), "wrong encoded input byte count");
    require(encode_stats.samples == 2048, "wrong encoded sample count");
    require(encode_stats.output_bytes > 0, "compressed file is empty");
    require(ldcompress::detect_flac_container(output_path.string()) == ldcompress::FlacContainer::Ogg,
        "default compression did not write Ogg FLAC");

    std::stringstream decoded;
    ldcompress::FileDigest compressed_digest;
    const auto decode_stats = ldcompress::decompress_flac_to_lds_with_input_digest(
        output_path.string(), decoded, compressed_digest);
    require(decode_stats.samples == 2048, "wrong decoded sample count");
    require(decode_stats.output_bytes == fixture.size(), "wrong decoded output byte count");
    require(decoded.str() == fixture, "FLAC round trip changed LDS bytes");
    const auto expected_digest = ldcompress::md5_file(output_path.string());
    require(compressed_digest.bytes == expected_digest.bytes,
        "Ogg digest decode read an unexpected number of compressed bytes");
    require(compressed_digest.md5.digest() == expected_digest.md5.digest(),
        "Ogg digest decode MD5 did not match md5_file");

    const auto second_path = temp_dir / "fixture-second.ldf";
    std::stringstream second_input(fixture);
    (void)ldcompress::compress_lds_to_flac(
        second_input, second_path.string(), ldcompress::FlacEncodeOptions {});
    const auto chained_path = temp_dir / "fixture-chained.ldf";
    {
        std::ifstream first_input(output_path, std::ios::binary);
        std::ifstream second_input_file(second_path, std::ios::binary);
        std::ofstream chained_output(chained_path, std::ios::binary);
        if (!first_input || !second_input_file || !chained_output) {
            throw std::runtime_error("could not construct chained Ogg FLAC test input");
        }
        chained_output << first_input.rdbuf() << second_input_file.rdbuf();
        if (!chained_output) {
            throw std::runtime_error("could not finish chained Ogg FLAC test input");
        }
    }

    std::stringstream chained_decoded;
    const auto chained_stats = ldcompress::decompress_flac_to_lds(
        chained_path.string(), chained_decoded);
    require(chained_stats.samples == 2048,
        "chained Ogg FLAC decoded more than the first link");
    require(chained_decoded.str() == fixture,
        "chained Ogg FLAC changed the first-link LDS bytes");

    std::stringstream chained_digest_decoded;
    ldcompress::FileDigest chained_digest;
    (void)ldcompress::decompress_flac_to_lds_with_input_digest(
        chained_path.string(), chained_digest_decoded, chained_digest);
    require(chained_digest_decoded.str() == fixture,
        "digest chained Ogg FLAC changed the first-link LDS bytes");
    const auto expected_chained_digest = ldcompress::md5_file(chained_path.string());
    require(chained_digest.bytes == expected_chained_digest.bytes,
        "digest chained Ogg FLAC did not cover the complete input");
    require(chained_digest.md5.digest() == expected_chained_digest.md5.digest(),
        "digest chained Ogg FLAC MD5 did not match md5_file");

    std::filesystem::remove(output_path);
    std::filesystem::remove(second_path);
    std::filesystem::remove(chained_path);
    std::filesystem::remove(temp_dir);
}

}  // namespace

int main()
{
    try {
        test_ogg_flac_round_trip();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_roundtrip: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
