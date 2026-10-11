#include "compressor.h"
#include "flac_codec.h"
#include "hash.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

class HashingStreambuf final : public std::streambuf {
public:
    std::uint64_t bytes() const { return bytes_; }
    const ldcompress::Md5& md5() const { return md5_; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        md5_.update(s, static_cast<std::uint64_t>(count));
        bytes_ += static_cast<std::uint64_t>(count);
        return count;
    }

    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }
        const auto byte = static_cast<unsigned char>(ch);
        md5_.update(&byte, 1);
        ++bytes_;
        return ch;
    }

private:
    ldcompress::Md5 md5_;
    std::uint64_t bytes_ = 0;
};

struct TimedStats {
    ldcompress::ConversionStats stats;
    ldcompress::NativeCompressionStats native_stats;
    bool show_native_stats = false;
    double elapsed_seconds = 0.0;
};

struct Fixture {
    std::filesystem::path path;
    std::filesystem::path relative_path;
};

class ScopedDirectory final {
public:
    explicit ScopedDirectory(std::filesystem::path path) : path_(std::move(path))
    {
        std::error_code ec;
        if (!std::filesystem::create_directory(path_, ec)) {
            throw std::runtime_error("could not create temporary fixture test directory: " +
                path_.string() + (ec ? ": " + ec.message() : ""));
        }
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    ~ScopedDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::filesystem::path make_temp_dir()
{
    const auto base = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base /
            ("ld-compress-ng-real-fixtures-" + std::to_string(::getpid()) + "-" +
                std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    throw std::runtime_error("could not allocate temporary fixture test directory");
}

std::vector<Fixture> find_lds_fixtures(const std::filesystem::path& root)
{
    std::vector<Fixture> fixtures;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lds") {
            continue;
        }

        std::error_code ec;
        auto relative = std::filesystem::relative(entry.path(), root, ec);
        if (ec) {
            relative = entry.path().filename();
        }
        fixtures.push_back(Fixture {
            .path = entry.path(),
            .relative_path = relative,
        });
    }

    std::sort(fixtures.begin(), fixtures.end(), [](const Fixture& lhs, const Fixture& rhs) {
        return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
    });
    return fixtures;
}

ldcompress::FileDigest decode_digest(const std::filesystem::path& compressed_path)
{
    HashingStreambuf decoded_buffer;
    std::ostream decoded(&decoded_buffer);
    const auto stats = ldcompress::decompress_flac_to_lds(compressed_path.string(), decoded);
    decoded.flush();
    require(static_cast<bool>(decoded), "failed to hash decoded output for " + compressed_path.string());
    require(stats.output_bytes == decoded_buffer.bytes(),
        "decoded byte accounting mismatch for " + compressed_path.string());

    ldcompress::FileDigest digest;
    digest.bytes = decoded_buffer.bytes();
    digest.md5 = decoded_buffer.md5();
    return digest;
}

void require_digest_match(
    const ldcompress::FileDigest& source,
    const ldcompress::FileDigest& decoded,
    const std::string& label)
{
    require(source.bytes == decoded.bytes, label + " decoded byte count changed");
    require(source.md5.digest() == decoded.md5.digest(), label + " decoded MD5 changed");
}

TimedStats compress_fixture(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    const ldcompress::CompressionOptions& options)
{
    std::ifstream input(input_path, std::ios::binary);
    require(static_cast<bool>(input), "could not open input fixture: " + input_path.string());

    const auto started = std::chrono::steady_clock::now();
    const auto stats = ldcompress::compress_lds(input, output_path.string(), options);
    const auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finished - started;

    return TimedStats {
        .stats = stats,
        .native_stats = options.native_stats == nullptr
            ? ldcompress::NativeCompressionStats {}
            : *options.native_stats,
        .show_native_stats = options.native_stats != nullptr,
        .elapsed_seconds = elapsed.count(),
    };
}

template <std::size_t N>
std::string summarize_nonzero_counts(
    const std::array<std::uint64_t, N>& counts,
    std::size_t max_items = 3)
{
    std::vector<std::pair<std::uint64_t, std::size_t>> nonzero;
    nonzero.reserve(counts.size());
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != 0) {
            nonzero.emplace_back(counts[i], i);
        }
    }
    if (nonzero.empty()) {
        return "-";
    }

    std::sort(nonzero.begin(), nonzero.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });

    std::ostringstream out;
    const auto limit = std::min(max_items, nonzero.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            out << ',';
        }
        out << nonzero[i].second << ':' << nonzero[i].first;
    }
    return out.str();
}

std::string summarize_subframes(const ldcompress::NativeCompressionStats& stats)
{
    if (stats.frames == 0) {
        return "-";
    }
    std::ostringstream out;
    out << "c" << stats.constant_frames
        << ",f" << stats.fixed_rice_frames
        << ",l" << stats.lpc_rice_frames
        << ",v" << stats.verbatim_frames;
    return out.str();
}

void print_result(
    const std::string& fixture_name,
    const std::string& backend,
    unsigned threads,
    const TimedStats& result)
{
    const double ratio = result.stats.input_bytes == 0
        ? 0.0
        : static_cast<double>(result.stats.output_bytes) /
            static_cast<double>(result.stats.input_bytes);
    const double mib_per_second = result.elapsed_seconds <= 0.0
        ? 0.0
        : (static_cast<double>(result.stats.input_bytes) / (1024.0 * 1024.0)) /
            result.elapsed_seconds;

    std::cout << std::left << std::setw(32) << fixture_name
              << std::setw(14) << backend
              << std::right << std::setw(8) << threads
              << std::setw(13) << result.stats.input_bytes
              << std::setw(13) << result.stats.output_bytes
              << std::setw(12) << result.stats.samples
              << std::setw(9) << std::fixed << std::setprecision(4) << ratio
              << std::setw(10) << std::fixed << std::setprecision(3) << result.elapsed_seconds
              << std::setw(10) << std::fixed << std::setprecision(2) << mib_per_second
              << std::setw(28) << (result.show_native_stats ? summarize_subframes(result.native_stats) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.lpc_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.partition_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.wasted_bits_counts) : "-")
              << '\n';
}

std::filesystem::path matching_legacy_ldf(const std::filesystem::path& lds_path)
{
    auto ldf_path = lds_path;
    ldf_path.replace_extension(".ldf");
    if (!std::filesystem::is_regular_file(ldf_path)) {
        return {};
    }
    if (ends_with(ldf_path.filename().string(), ".flac.ldf")) {
        return {};
    }
    return ldf_path;
}

void print_header()
{
    std::cout << std::left << std::setw(32) << "fixture"
              << std::setw(14) << "backend"
              << std::right << std::setw(8) << "threads"
              << std::setw(13) << "input_bytes"
              << std::setw(13) << "output_bytes"
              << std::setw(12) << "samples"
              << std::setw(9) << "ratio"
              << std::setw(10) << "elapsed_s"
              << std::setw(10) << "mib_s"
              << std::setw(28) << "subframes"
              << std::setw(24) << "lpc_orders"
              << std::setw(24) << "rice_orders"
              << std::setw(24) << "wasted_bits"
              << '\n';
}

void test_real_fixtures(const std::filesystem::path& root)
{
    require(std::filesystem::is_directory(root), "fixture root is not a directory: " + root.string());

    const auto fixtures = find_lds_fixtures(root);
    require(!fixtures.empty(), "no .lds fixtures found under " + root.string());

    ScopedDirectory temp_dir(make_temp_dir());
    print_header();

    std::uint64_t legacy_ldf_count = 0;
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto& fixture = fixtures[i];
        const auto fixture_name = fixture.relative_path.generic_string();
        const auto source_digest = ldcompress::md5_file(fixture.path.string());

        const auto legacy_path = matching_legacy_ldf(fixture.path);
        if (!legacy_path.empty()) {
            const auto decoded = decode_digest(legacy_path);
            require_digest_match(source_digest, decoded, fixture_name + " legacy .ldf");
            ++legacy_ldf_count;
        }

        const auto cpu_output = temp_dir.path() / ("case-" + std::to_string(i) + ".ldf");
        const ldcompress::CompressionOptions cpu_options {
            .backend = ldcompress::CompressionBackend::CpuLibFlac,
            .container = ldcompress::FlacContainer::Ogg,
            .compression_level = 11,
            .sample_rate = 40000,
        };
        const auto cpu_result = compress_fixture(fixture.path, cpu_output, cpu_options);
        require_digest_match(source_digest, decode_digest(cpu_output), fixture_name + " cpu");
        print_result(fixture_name, "cpu", 1, cpu_result);

        const auto native_output = temp_dir.path() / ("case-" + std::to_string(i) + ".flac.ldf");
        ldcompress::NativeCompressionStats native_stats;
        const ldcompress::CompressionOptions native_options {
            .backend = ldcompress::CompressionBackend::NativeFixedFlac,
            .container = ldcompress::FlacContainer::Native,
            .compression_level = 11,
            .sample_rate = 40000,
            .thread_count = 8,
            .native_frame_samples = 4608,
            .native_max_lpc_order = 12,
            .native_stats = &native_stats,
        };
        const auto native_result = compress_fixture(fixture.path, native_output, native_options);
        require_digest_match(source_digest, decode_digest(native_output), fixture_name + " native-fixed");
        print_result(fixture_name, "native-fixed", 8, native_result);
    }

    const auto smallest = std::min_element(
        fixtures.begin(), fixtures.end(), [](const Fixture& lhs, const Fixture& rhs) {
            return std::filesystem::file_size(lhs.path) < std::filesystem::file_size(rhs.path);
        });
    require(smallest != fixtures.end(), "could not choose threaded parity fixture");

    const ldcompress::CompressionOptions single_options {
        .backend = ldcompress::CompressionBackend::NativeFixedFlac,
        .container = ldcompress::FlacContainer::Native,
        .compression_level = 11,
        .sample_rate = 40000,
        .thread_count = 1,
        .native_frame_samples = 4608,
        .native_max_lpc_order = 12,
    };
    const ldcompress::CompressionOptions threaded_options {
        .backend = ldcompress::CompressionBackend::NativeFixedFlac,
        .container = ldcompress::FlacContainer::Native,
        .compression_level = 11,
        .sample_rate = 40000,
        .thread_count = 8,
        .native_frame_samples = 4608,
        .native_max_lpc_order = 12,
    };

    const auto single_output = temp_dir.path() / "thread-parity-single.flac.ldf";
    const auto threaded_output = temp_dir.path() / "thread-parity-threaded.flac.ldf";
    compress_fixture(smallest->path, single_output, single_options);
    compress_fixture(smallest->path, threaded_output, threaded_options);
    const auto single_digest = ldcompress::md5_file(single_output.string());
    const auto threaded_digest = ldcompress::md5_file(threaded_output.string());
    require_digest_match(single_digest, threaded_digest,
        smallest->relative_path.generic_string() + " native threaded parity");

    std::cout << "checked " << fixtures.size() << " LDS fixtures";
    if (legacy_ldf_count != 0) {
        std::cout << " and " << legacy_ldf_count << " matching legacy .ldf files";
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("expected path to real LDS fixture directory");
        }
        test_real_fixtures(argv[1]);
    } catch (const std::exception& ex) {
        std::cerr << "test_real_fixtures: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
