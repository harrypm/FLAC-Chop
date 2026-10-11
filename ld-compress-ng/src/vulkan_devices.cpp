#include "vulkan_devices.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef LDCOMPRESS_HAVE_VULKAN
#define LDCOMPRESS_HAVE_VULKAN 0
#endif

#if LDCOMPRESS_HAVE_VULKAN
#include <vulkan/vulkan.h>

#include <vector>
#endif

namespace ldcompress {
namespace {

#if LDCOMPRESS_HAVE_VULKAN

std::string vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    default:
        return "Vulkan error " + std::to_string(static_cast<int>(result));
    }
}

void require_vk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + vk_result_name(result));
    }
}

bool missing_runtime_result(VkResult result)
{
    return result == VK_ERROR_INCOMPATIBLE_DRIVER ||
        result == VK_ERROR_INITIALIZATION_FAILED;
}

class Instance final {
public:
    explicit Instance(bool allow_missing_runtime)
    {
        const VkApplicationInfo app_info {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "ld-compress-ng",
            .applicationVersion = VK_MAKE_VERSION(1, 1, 0),
            .pEngineName = "ld-compress-ng",
            .engineVersion = VK_MAKE_VERSION(1, 1, 0),
            .apiVersion = VK_API_VERSION_1_0,
        };
        const VkInstanceCreateInfo create_info {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pApplicationInfo = &app_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = 0,
            .ppEnabledExtensionNames = nullptr,
        };
        const VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
        if (result == VK_SUCCESS) {
            return;
        }
        if (allow_missing_runtime && missing_runtime_result(result)) {
            instance_ = VK_NULL_HANDLE;
            return;
        }
        require_vk(result, "vkCreateInstance");
    }

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    ~Instance()
    {
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VkInstance get() const { return instance_; }
    bool valid() const { return instance_ != VK_NULL_HANDLE; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
};

std::string version_string(std::uint32_t version)
{
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version) << '.'
        << VK_VERSION_MINOR(version) << '.'
        << VK_VERSION_PATCH(version);
    return out.str();
}

std::string device_type_name(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "unknown";
    }
}

std::string vendor_name(std::uint32_t vendor_id)
{
    switch (vendor_id) {
    case 0x1002:
    case 0x1022:
        return "AMD";
    case 0x10de:
        return "NVIDIA";
    case 0x8086:
        return "Intel";
    case 0x13b5:
        return "Arm";
    case 0x5143:
        return "Qualcomm";
    default: {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(4) << std::setfill('0') << vendor_id;
        return out.str();
    }
    }
}

std::uint64_t device_local_memory_bytes(VkPhysicalDevice device)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(device, &memory_properties);

    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
        const auto& heap = memory_properties.memoryHeaps[i];
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }
        if (heap.size > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        total += heap.size;
    }
    return total;
}

std::vector<VkPhysicalDevice> enumerate_physical_devices(VkInstance instance)
{
    std::uint32_t count = 0;
    const VkResult count_result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count_result == VK_ERROR_INITIALIZATION_FAILED || count == 0) {
        return {};
    }
    require_vk(count_result, "vkEnumeratePhysicalDevices");

    std::vector<VkPhysicalDevice> devices(count);
    require_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
        "vkEnumeratePhysicalDevices");
    devices.resize(count);
    return devices;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> compute_queue_family(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0) {
        return std::nullopt;
    }

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            families[i].queueCount > 0) {
            return std::make_pair(i, families[i].queueCount);
        }
    }
    return std::nullopt;
}

#endif

}  // namespace

bool vulkan_support_built()
{
    return LDCOMPRESS_HAVE_VULKAN != 0;
}

std::vector<VulkanDeviceInfo> list_vulkan_devices()
{
#if LDCOMPRESS_HAVE_VULKAN
    Instance instance(true);
    if (!instance.valid()) {
        return {};
    }
    const auto physical_devices = enumerate_physical_devices(instance.get());

    std::vector<VulkanDeviceInfo> devices;
    devices.reserve(physical_devices.size());
    for (const auto physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical_device, &properties);

        const auto compute_family = compute_queue_family(physical_device);
        VulkanDeviceInfo info;
        info.index = static_cast<std::uint32_t>(devices.size());
        info.device_name = properties.deviceName;
        info.device_type = device_type_name(properties.deviceType);
        info.vendor_name = vendor_name(properties.vendorID);
        info.vendor_id = properties.vendorID;
        info.device_id = properties.deviceID;
        info.api_version = version_string(properties.apiVersion);
        info.driver_version = properties.driverVersion;
        if (compute_family.has_value()) {
            info.compute_queue_family_index = compute_family->first;
            info.compute_queue_count = compute_family->second;
            info.available = true;
        }
        VkPhysicalDeviceFeatures features {};
        vkGetPhysicalDeviceFeatures(physical_device, &features);
        info.shader_int64 = features.shaderInt64 == VK_TRUE;
        info.max_compute_work_group_invocations =
            properties.limits.maxComputeWorkGroupInvocations;
        info.max_compute_shared_memory_bytes =
            properties.limits.maxComputeSharedMemorySize;
        info.device_local_memory_bytes = device_local_memory_bytes(physical_device);
        devices.push_back(std::move(info));
    }
    return devices;
#else
    return {};
#endif
}

VulkanDeviceInfo select_vulkan_device(std::optional<std::size_t> requested_index)
{
    if (!vulkan_support_built()) {
        throw std::runtime_error("Vulkan support was not built");
    }

    const auto devices = list_vulkan_devices();
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }

    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("Vulkan device index out of range: " +
                std::to_string(*requested_index) + " (visible devices: " +
                std::to_string(devices.size()) + "; run `ld-compress-ng devices`)");
        }
        if (!devices[*requested_index].available) {
            throw std::runtime_error("selected Vulkan device has no compute queue: " +
                devices[*requested_index].device_name);
        }
        return devices[*requested_index];
    }

    for (const auto& device : devices) {
        if (device.available) {
            return device;
        }
    }

    throw std::runtime_error("no available Vulkan compute devices found");
}

}  // namespace ldcompress
