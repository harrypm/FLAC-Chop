#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldcompress {

struct VulkanDeviceInfo {
    std::uint32_t index = 0;
    std::string device_name;
    std::string device_type;
    std::string vendor_name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string api_version;
    std::uint32_t driver_version = 0;
    std::uint32_t compute_queue_family_index = 0;
    std::uint32_t compute_queue_count = 0;
    std::uint32_t max_compute_work_group_invocations = 0;
    std::uint64_t max_compute_shared_memory_bytes = 0;
    std::uint64_t device_local_memory_bytes = 0;
    bool shader_int64 = false;
    bool available = false;
};

bool vulkan_support_built();
std::vector<VulkanDeviceInfo> list_vulkan_devices();
VulkanDeviceInfo select_vulkan_device(std::optional<std::size_t> requested_index);

}  // namespace ldcompress
