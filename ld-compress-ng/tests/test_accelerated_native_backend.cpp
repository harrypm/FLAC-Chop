#include "accelerated_native_backend.h"
#include "lds_codec.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string make_two_frame_lds(unsigned frame_samples)
{
    const auto total_samples = static_cast<std::size_t>(frame_samples) * 2U;
    require((total_samples % 4U) == 0, "test sample count was not LDS-aligned");

    std::string packed;
    packed.reserve((total_samples / 4U) * 5U);
    for (std::size_t offset = 0; offset < total_samples; offset += 4U) {
        ldcompress::SampleGroup group {};
        for (std::size_t i = 0; i < group.size(); ++i) {
            const auto sample_index = offset + i;
            const auto value =
                static_cast<int>((sample_index * 37U) % 1024U) - 512;
            group[i] = static_cast<std::int16_t>(value * 64);
        }
        const auto bytes = ldcompress::pack_group(group);
        packed.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return packed;
}

ldcompress::AcceleratedSelectedFrameAnalysis make_failing_threaded_analysis()
{
    ldcompress::AcceleratedSelectedFrameAnalysis analysis;

    ldcompress::FlacSelectedSubframe slow_lpc {
        .kind = ldcompress::FlacSubframeKind::LpcRice,
        .lpc_order = 32,
        .rice_partition_order = 0,
        .wasted_bits = 6,
        .coefficient_precision = 15,
        .quantization_shift = 0,
        .coefficients = std::vector<std::int32_t>(32, 0),
        .rice_parameters = {8},
    };
    ldcompress::FlacSubframeDecision slow_lpc_decision {
        .kind = ldcompress::FlacSubframeKind::LpcRice,
        .lpc_order = 32,
        .rice_partition_order = 0,
        .wasted_bits = 6,
        .estimated_bits = 1,
    };

    ldcompress::FlacSelectedSubframe bad_constant {
        .kind = ldcompress::FlacSubframeKind::Constant,
        .wasted_bits = 5,
    };
    ldcompress::FlacSubframeDecision bad_constant_decision {
        .kind = ldcompress::FlacSubframeKind::Constant,
        .wasted_bits = 5,
        .estimated_bits = 1,
    };

    analysis.selected_subframes.push_back(std::move(slow_lpc));
    analysis.decisions.push_back(slow_lpc_decision);
    analysis.selected_subframes.push_back(bad_constant);
    analysis.decisions.push_back(bad_constant_decision);
    return analysis;
}

void test_threaded_writer_jobs_keep_batch_samples_alive_on_error()
{
    // Keep the successful worker inside selected-frame validation long enough
    // for the second worker to fail and unwind the analyzed batch.  This is
    // still within FLAC's 16-bit maximum block-size field.
    constexpr unsigned kFrameSamples = 65532;
    const auto temp_dir = std::filesystem::temp_directory_path() /
        ("ld-compress-ng-accelerated-lifetime-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directory(temp_dir);
    const auto output_path = temp_dir / "writer-error.flac";

    std::istringstream input(make_two_frame_lds(kFrameSamples));
    const ldcompress::AcceleratedNativeCompressionOptions options {
        .backend_label = "test-accelerated",
        .container = ldcompress::FlacContainer::Native,
        .sample_rate = 40000,
        .thread_count = 2,
        .frame_samples = kFrameSamples,
        .max_lpc_order = 32,
        .lpc_precision = 15,
        .max_rice_partition_order = 0,
        .batch_frames = 2,
    };

    std::string error_message;
    try {
        (void)ldcompress::compress_lds_to_accelerated_native_flac(
            input,
            output_path.string(),
            options,
            [](const std::vector<std::int32_t>& samples,
                const ldcompress::FlacFrameInfo&,
                unsigned frame_samples) {
                if (samples.size() != static_cast<std::size_t>(frame_samples) * 2U) {
                    throw std::runtime_error("test analyzer received the wrong batch shape");
                }
                return make_failing_threaded_analysis();
            });
    } catch (const std::runtime_error& ex) {
        error_message = ex.what();
    }
    require(
        error_message ==
            "selected FLAC subframe wasted-bits count does not match samples",
        "threaded accelerated writer did not propagate the expected worker error");

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main()
{
    try {
        test_threaded_writer_jobs_keep_batch_samples_alive_on_error();
    } catch (const std::exception& ex) {
        std::cerr << "test_accelerated_native_backend: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
