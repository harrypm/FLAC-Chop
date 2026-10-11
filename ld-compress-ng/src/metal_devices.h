#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldcompress {

struct MetalDeviceInfo {
    std::uint32_t index = 0;
    std::string device_name;
    bool available = false;
    bool low_power = false;
    bool removable = false;
    bool unified_memory = false;
    std::uint64_t recommended_max_working_set_bytes = 0;
    std::uint64_t registry_id = 0;
    std::uint64_t max_buffer_length = 0;
    std::uint64_t max_threads_per_threadgroup = 0;
};

bool metal_support_built();
std::vector<MetalDeviceInfo> list_metal_devices();
MetalDeviceInfo select_metal_device(std::optional<std::size_t> requested_index);

}  // namespace ldcompress
