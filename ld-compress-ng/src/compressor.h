#pragma once

#include "flac_codec.h"
#include "lds_codec.h"
#include "native_analysis_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

namespace ldcompress {

enum class CompressionBackend {
    CpuLibFlac,
    NativeVerbatimFlac,
    NativeFixedFlac,
    OpenClNativeFlac,
    VulkanNativeFlac,
    MetalNativeFlac,
};

struct NativeCompressionStats {
    std::uint64_t frames = 0;
    std::uint64_t constant_frames = 0;
    std::uint64_t verbatim_frames = 0;
    std::uint64_t fixed_rice_frames = 0;
    std::uint64_t lpc_rice_frames = 0;
    std::uint64_t estimated_subframe_bits = 0;
    std::array<std::uint64_t, 5> fixed_order_counts {};
    std::array<std::uint64_t, 33> lpc_order_counts {};
    std::array<std::uint64_t, 16> partition_order_counts {};
    std::array<std::uint64_t, 33> wasted_bits_counts {};
    std::uint64_t accelerated_batches = 0;
    std::uint64_t accelerated_total_ns = 0;
    std::uint64_t accelerated_setup_ns = 0;
    std::uint64_t accelerated_scan_ns = 0;
    std::uint64_t accelerated_scan_read_ns = 0;
    std::uint64_t accelerated_scan_decode_ns = 0;
    std::uint64_t accelerated_scan_md5_ns = 0;
    std::uint64_t accelerated_analyzer_ns = 0;
    std::uint64_t accelerated_selected_write_ns = 0;
    std::uint64_t accelerated_tail_write_ns = 0;
    std::uint64_t accelerated_selected_validation_ns = 0;
    std::uint64_t accelerated_selected_shift_ns = 0;
    std::uint64_t accelerated_selected_residual_ns = 0;
    std::uint64_t accelerated_selected_rice_parameter_ns = 0;
    std::uint64_t accelerated_selected_bitstream_ns = 0;
    std::uint64_t accelerated_selected_frame_output_ns = 0;
    std::uint64_t accelerated_task_plan_ns = 0;
    std::uint64_t accelerated_exact_analysis_ns = 0;
    std::uint64_t opencl_task_plan_fixed_guess_ns = 0;
    std::uint64_t opencl_task_plan_fill_ns = 0;
    std::uint64_t opencl_setup_device_ns = 0;
    std::uint64_t opencl_setup_context_ns = 0;
    std::uint64_t opencl_setup_queue_ns = 0;
    std::uint64_t opencl_setup_program_source_ns = 0;
    std::uint64_t opencl_setup_program_build_ns = 0;
    std::uint64_t opencl_setup_kernels_ns = 0;
    std::uint64_t opencl_timed_batches = 0;
    std::uint64_t opencl_upload_ns = 0;
    std::uint64_t opencl_wasted_bits_ns = 0;
    std::uint64_t opencl_generated_autocorrelation_ns = 0;
    std::uint64_t opencl_generated_lpc_ns = 0;
    std::uint64_t opencl_generated_quantize_ns = 0;
    std::uint64_t opencl_fixed_order_guess_ns = 0;
    std::uint64_t opencl_exact_analysis_ns = 0;
    std::uint64_t opencl_choose_best_ns = 0;
    std::uint64_t opencl_readback_ns = 0;
    std::uint64_t vulkan_gpu_timed_batches = 0;
    std::uint64_t vulkan_gpu_total_ns = 0;
    std::uint64_t vulkan_gpu_upload_ns = 0;
    std::uint64_t vulkan_gpu_prepare_ns = 0;
    std::uint64_t vulkan_gpu_generated_autocorrelation_ns = 0;
    std::uint64_t vulkan_gpu_generated_lpc_ns = 0;
    std::uint64_t vulkan_gpu_generated_quantize_ns = 0;
    std::uint64_t vulkan_gpu_fixed_order_guess_ns = 0;
    std::uint64_t vulkan_gpu_exact_analysis_ns = 0;
    std::uint64_t vulkan_gpu_choose_best_ns = 0;
    std::uint64_t vulkan_gpu_readback_ns = 0;
    std::uint64_t metal_timed_batches = 0;
    std::uint64_t metal_upload_ns = 0;
    std::uint64_t metal_generated_total_ns = 0;
    std::uint64_t metal_wasted_bits_ns = 0;
    std::uint64_t metal_generated_autocorrelation_ns = 0;
    std::uint64_t metal_generated_lpc_ns = 0;
    std::uint64_t metal_generated_quantize_ns = 0;
    std::uint64_t metal_fixed_order_guess_ns = 0;
    std::uint64_t metal_exact_analysis_ns = 0;
    std::uint64_t metal_choose_best_ns = 0;
    std::uint64_t metal_readback_ns = 0;
};

struct CompressionOptions {
    CompressionBackend backend = CompressionBackend::CpuLibFlac;
    FlacContainer container = FlacContainer::Ogg;
    unsigned compression_level = 11;
    unsigned sample_rate = 40000;
    unsigned thread_count = 1;
    unsigned native_frame_samples = 4608;
    unsigned native_max_lpc_order = 12;
    unsigned native_lpc_precision = 12;
    unsigned native_max_rice_partition_order = 5;
    NativeAnalysisProfile native_analysis_profile = NativeAnalysisProfile::Exact;
    NativeCompressionStats* native_stats = nullptr;
    CompressionProgressCallback progress_callback;
    std::optional<std::size_t> opencl_device_index;
    std::optional<std::size_t> vulkan_device_index;
    std::optional<std::size_t> metal_device_index;
};

const char* backend_name(CompressionBackend backend);

ConversionStats compress_lds(
    std::istream& lds_input,
    const std::string& output_path,
    const CompressionOptions& options);

}  // namespace ldcompress
