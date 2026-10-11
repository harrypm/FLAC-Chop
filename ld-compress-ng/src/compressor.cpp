#include "compressor.h"

#include "metal_backend.h"
#include "native_flac_encoder.h"
#include "opencl_backend.h"
#include "vulkan_backend.h"

#include <stdexcept>

namespace ldcompress {

const char* backend_name(CompressionBackend backend)
{
    switch (backend) {
    case CompressionBackend::CpuLibFlac:
        return "cpu";
    case CompressionBackend::NativeVerbatimFlac:
        return "native-verbatim";
    case CompressionBackend::NativeFixedFlac:
        return "native-fixed";
    case CompressionBackend::OpenClNativeFlac:
        return "opencl";
    case CompressionBackend::VulkanNativeFlac:
        return "vulkan";
    case CompressionBackend::MetalNativeFlac:
        return "metal";
    }
    return "unknown";
}

ConversionStats compress_lds(
    std::istream& lds_input,
    const std::string& output_path,
    const CompressionOptions& options)
{
    switch (options.backend) {
    case CompressionBackend::CpuLibFlac: {
        const FlacEncodeOptions flac_options {
            .container = options.container,
            .compression_level = options.compression_level,
            .sample_rate = options.sample_rate,
            .progress_callback = options.progress_callback,
        };
        return compress_lds_to_flac(lds_input, output_path, flac_options);
    }
    case CompressionBackend::NativeVerbatimFlac:
        if (options.container != FlacContainer::Native) {
            throw std::runtime_error("native-verbatim backend writes native FLAC only");
        }
        return compress_lds_to_native_verbatim_flac(
            lds_input, output_path, options.sample_rate, options.thread_count,
            options.native_frame_samples, options.native_max_lpc_order,
            options.native_lpc_precision,
            options.native_max_rice_partition_order,
            options.native_stats,
            options.progress_callback);
    case CompressionBackend::NativeFixedFlac:
        if (options.container != FlacContainer::Native) {
            throw std::runtime_error("native-fixed backend writes native FLAC only");
        }
        return compress_lds_to_native_fixed_flac(
            lds_input, output_path, options.sample_rate, options.thread_count,
            options.native_frame_samples, options.native_max_lpc_order,
            options.native_lpc_precision,
            options.native_max_rice_partition_order,
            options.native_analysis_profile,
            options.native_stats,
            options.progress_callback);
    case CompressionBackend::OpenClNativeFlac:
        return compress_lds_to_opencl_native_flac(lds_input, output_path, OpenClCompressionOptions {
            .container = options.container,
            .sample_rate = options.sample_rate,
            .thread_count = options.thread_count,
            .frame_samples = options.native_frame_samples,
            .max_lpc_order = options.native_max_lpc_order,
            .lpc_precision = options.native_lpc_precision,
            .max_rice_partition_order = options.native_max_rice_partition_order,
            .analysis_profile = options.native_analysis_profile,
            .device_index = options.opencl_device_index,
            .native_stats = options.native_stats,
            .progress_callback = options.progress_callback,
        });
    case CompressionBackend::VulkanNativeFlac:
        return compress_lds_to_vulkan_native_flac(lds_input, output_path, VulkanCompressionOptions {
            .container = options.container,
            .sample_rate = options.sample_rate,
            .thread_count = options.thread_count,
            .frame_samples = options.native_frame_samples,
            .max_lpc_order = options.native_max_lpc_order,
            .lpc_precision = options.native_lpc_precision,
            .max_rice_partition_order = options.native_max_rice_partition_order,
            .analysis_profile = options.native_analysis_profile,
            .device_index = options.vulkan_device_index,
            .native_stats = options.native_stats,
            .progress_callback = options.progress_callback,
        });
    case CompressionBackend::MetalNativeFlac:
        return compress_lds_to_metal_native_flac(lds_input, output_path, MetalCompressionOptions {
            .container = options.container,
            .sample_rate = options.sample_rate,
            .thread_count = options.thread_count,
            .frame_samples = options.native_frame_samples,
            .max_lpc_order = options.native_max_lpc_order,
            .lpc_precision = options.native_lpc_precision,
            .max_rice_partition_order = options.native_max_rice_partition_order,
            .analysis_profile = options.native_analysis_profile,
            .device_index = options.metal_device_index,
            .native_stats = options.native_stats,
            .progress_callback = options.progress_callback,
        });
    }

    throw std::runtime_error("unknown compression backend");
}

}  // namespace ldcompress
