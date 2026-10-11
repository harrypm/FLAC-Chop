#pragma once

#include "flac_codec.h"
#include "flac_native_writer.h"
#include "lds_codec.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace ldcompress {

struct NativeCompressionStats;

struct AcceleratedNativeCompressionOptions {
    const char* backend_label = "accelerated";
    FlacContainer container = FlacContainer::Native;
    unsigned sample_rate = 40000;
    unsigned thread_count = 1;
    unsigned frame_samples = 4608;
    unsigned max_lpc_order = 12;
    unsigned lpc_precision = 12;
    unsigned max_rice_partition_order = 5;
    std::size_t batch_frames = 32;
    NativeCompressionStats* native_stats = nullptr;
    CompressionProgressCallback progress_callback;
};

struct AcceleratedSelectedFrameAnalysis {
    std::vector<FlacSubframeDecision> decisions;
    std::vector<FlacSelectedSubframe> selected_subframes;
};

using AcceleratedBatchAnalyzer = std::function<AcceleratedSelectedFrameAnalysis(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& base_frame_info,
    unsigned frame_samples)>;

ConversionStats compress_lds_to_accelerated_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer);

}  // namespace ldcompress
