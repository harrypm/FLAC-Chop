#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldcompress {

struct OpenClDeviceInfo {
    std::uint32_t flat_index = 0;
    std::uint32_t platform_index = 0;
    std::uint32_t device_index = 0;
    std::string platform_name;
    std::string platform_vendor;
    std::string platform_version;
    std::string device_name;
    std::string device_vendor;
    std::string device_version;
    std::string driver_version;
    std::string type;
    std::uint32_t compute_units = 0;
    std::uint64_t global_memory_bytes = 0;
    // True for OpenCL GPU or accelerator-class devices. Auto backend selection
    // deliberately excludes CPU-only OpenCL implementations.
    bool hardware_accelerator = false;
    bool available = false;
};

bool opencl_support_built();
std::vector<OpenClDeviceInfo> list_opencl_devices();
OpenClDeviceInfo select_opencl_device(std::optional<std::size_t> requested_index);
bool opencl_device_is_auto_eligible(const OpenClDeviceInfo& device) noexcept;

}  // namespace ldcompress
