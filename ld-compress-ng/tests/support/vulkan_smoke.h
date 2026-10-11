#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldcompress {

struct VulkanComputeSmokeResult {
    std::uint32_t device_index = 0;
    std::string device_name;
    std::vector<std::uint32_t> values;
};

VulkanComputeSmokeResult run_vulkan_compute_smoke(
    const std::string& spirv_path,
    std::optional<std::size_t> requested_device_index);

}  // namespace ldcompress
