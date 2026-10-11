#include "flac_codec.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool contains(std::string_view value, std::string_view needle)
{
    return value.find(needle) != std::string_view::npos;
}

std::filesystem::path fixture_path(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_path)
{
    const auto path = root / relative_path;
    require(std::filesystem::is_regular_file(path), "missing FLAC testbench fixture: " + path.string());
    return path;
}

std::string rejection_message(const std::filesystem::path& path)
{
    try {
        std::ostringstream decoded;
        (void)ldcompress::decompress_flac_to_lds(path.string(), decoded);
    } catch (const std::runtime_error& ex) {
        return ex.what();
    }
    throw std::runtime_error("FLAC testbench fixture was unexpectedly accepted: " + path.string());
}

void require_rejected(const std::filesystem::path& path)
{
    (void)rejection_message(path);
}

void require_rejected_with(const std::filesystem::path& path, std::string_view expected)
{
    const auto message = rejection_message(path);
    require(contains(message, expected),
        "unexpected rejection for " + path.string() + ": " + message);
}

void test_reference_flac_rejections(const std::filesystem::path& root)
{
    const auto mono_44100_16 = fixture_path(root, "subset/60 - mono audio.flac");
    const auto mono_44100_20 = fixture_path(root, "subset/62 - predictor overflow check, 20-bit.flac");
    const auto three_channel = fixture_path(root, "subset/38 - 3 channels (3.0).flac");
    const auto wrong_streaminfo_depth = fixture_path(root, "faulty/03 - wrong bit depth.flac");
    const auto wrong_streaminfo_channels = fixture_path(root, "faulty/04 - wrong number of channels.flac");
    const auto missing_streaminfo = fixture_path(root, "faulty/06 - missing streaminfo metadata block.flac");
    const auto delayed_streaminfo = fixture_path(root,
        "faulty/07 - other metadata blocks preceding streaminfo metadata block.flac");
    const auto raw_frame_stream = fixture_path(root, "uncommon/10 - file starting at frame header.flac");
    const auto junk_then_frame_stream = fixture_path(root, "uncommon/11 - file starting with unparsable data.flac");

    require(ldcompress::detect_flac_container(mono_44100_16.string()) == ldcompress::FlacContainer::Native,
        "testbench native FLAC fixture was not detected as native FLAC");

    require_rejected_with(mono_44100_16, "sample rate");
    require_rejected_with(mono_44100_20, "16-bit");
    require_rejected_with(three_channel, "mono");
    require_rejected_with(wrong_streaminfo_depth, "16-bit");
    require_rejected_with(wrong_streaminfo_channels, "mono");
    require_rejected(missing_streaminfo);
    require_rejected(delayed_streaminfo);
    require_rejected_with(raw_frame_stream, "unsupported compressed input container");
    require_rejected_with(junk_then_frame_stream, "unsupported compressed input container");
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("expected path to FLAC decoder testbench directory");
        }
        test_reference_flac_rejections(argv[1]);
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_test_files: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
