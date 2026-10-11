#pragma once

#include "flac_codec.h"
#include "lds_codec.h"
#include "native_analysis_profile.h"

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace ldcompress {

struct NativeCompressionStats;

namespace metal_detail {
class MetalMonoAnalysisSession;
}

struct MetalCompressionOptions {
    FlacContainer container = FlacContainer::Native;
    unsigned sample_rate = 40000;
    unsigned thread_count = 1;
    unsigned frame_samples = 4608;
    unsigned max_lpc_order = 12;
    unsigned lpc_precision = 12;
    unsigned max_rice_partition_order = 5;
    NativeAnalysisProfile analysis_profile = NativeAnalysisProfile::Exact;
    std::optional<std::size_t> device_index;
    NativeCompressionStats* native_stats = nullptr;
    CompressionProgressCallback progress_callback;
};

class MetalCompressionSession final {
public:
    explicit MetalCompressionSession(
        std::optional<std::size_t> requested_device_index = std::nullopt);
    ~MetalCompressionSession();

    MetalCompressionSession(const MetalCompressionSession&) = delete;
    MetalCompressionSession& operator=(const MetalCompressionSession&) = delete;

    std::size_t device_index() const noexcept { return device_index_; }

    ConversionStats compress_lds_to_native_flac(
        std::istream& lds_input,
        const std::string& output_path,
        const MetalCompressionOptions& options);

private:
    std::size_t device_index_ = 0;
    std::unique_ptr<metal_detail::MetalMonoAnalysisSession> analysis_session_;
};

ConversionStats compress_lds_to_metal_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const MetalCompressionOptions& options);

}  // namespace ldcompress
