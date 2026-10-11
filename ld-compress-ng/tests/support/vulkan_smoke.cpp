#include "vulkan_smoke.h"

#include "vulkan_devices.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef LDCOMPRESS_HAVE_VULKAN
#define LDCOMPRESS_HAVE_VULKAN 0
#endif

#if LDCOMPRESS_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace ldcompress {
namespace {

#if LDCOMPRESS_HAVE_VULKAN

constexpr std::uint32_t kSmokeValueCount = 64;
constexpr VkDeviceSize kSmokeBufferBytes = kSmokeValueCount * sizeof(std::uint32_t);
constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ULL;

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

template <typename Fn>
class ScopeExit final {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit()
    {
        if (active_) {
            fn_();
        }
    }

    void release() { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit<Fn> scope_exit(Fn fn)
{
    return ScopeExit<Fn>(std::move(fn));
}

std::vector<std::uint32_t> read_spirv_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open Vulkan smoke shader: " + path);
    }
    const auto size = input.tellg();
    if (size <= 0 || (static_cast<std::uint64_t>(size) % sizeof(std::uint32_t)) != 0) {
        throw std::runtime_error("Vulkan smoke shader is not valid SPIR-V word data: " + path);
    }
    input.seekg(0);

    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(size) / sizeof(std::uint32_t));
    input.read(reinterpret_cast<char*>(words.data()), size);
    if (!input) {
        throw std::runtime_error("could not read Vulkan smoke shader: " + path);
    }
    return words;
}

VkInstance create_instance()
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

    VkInstance instance = VK_NULL_HANDLE;
    require_vk(vkCreateInstance(&create_info, nullptr, &instance), "vkCreateInstance");
    return instance;
}

std::vector<VkPhysicalDevice> enumerate_physical_devices(VkInstance instance)
{
    std::uint32_t count = 0;
    require_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
        "vkEnumeratePhysicalDevices");
    if (count == 0) {
        return {};
    }

    std::vector<VkPhysicalDevice> devices(count);
    require_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
        "vkEnumeratePhysicalDevices");
    devices.resize(count);
    return devices;
}

std::optional<std::uint32_t> compute_queue_family(VkPhysicalDevice device)
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
            return i;
        }
    }
    return std::nullopt;
}

struct SelectedDevice {
    std::uint32_t index = 0;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::uint32_t queue_family_index = 0;
    std::string name;
};

SelectedDevice select_physical_device(
    VkInstance instance,
    std::optional<std::size_t> requested_device_index)
{
    const auto devices = enumerate_physical_devices(instance);
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }

    auto make_selection = [&](std::size_t index) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(devices[index], &properties);
        const auto queue_family = compute_queue_family(devices[index]);
        if (!queue_family.has_value()) {
            throw std::runtime_error("selected Vulkan device has no compute queue: " +
                std::string(properties.deviceName));
        }
        return SelectedDevice {
            .index = static_cast<std::uint32_t>(index),
            .physical_device = devices[index],
            .queue_family_index = *queue_family,
            .name = properties.deviceName,
        };
    };

    if (requested_device_index.has_value()) {
        if (*requested_device_index >= devices.size()) {
            throw std::runtime_error("Vulkan device index out of range: " +
                std::to_string(*requested_device_index));
        }
        return make_selection(*requested_device_index);
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (compute_queue_family(devices[i]).has_value()) {
            return make_selection(i);
        }
    }
    throw std::runtime_error("no available Vulkan compute devices found");
}

std::uint32_t find_memory_type(
    VkPhysicalDevice physical_device,
    std::uint32_t type_bits,
    VkMemoryPropertyFlags required_flags,
    VkMemoryPropertyFlags preferred_flags)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    std::optional<std::uint32_t> fallback;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1U << i)) == 0) {
            continue;
        }
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required_flags) != required_flags) {
            continue;
        }
        if ((flags & preferred_flags) == preferred_flags) {
            return i;
        }
        if (!fallback.has_value()) {
            fallback = i;
        }
    }

    if (fallback.has_value()) {
        return *fallback;
    }
    throw std::runtime_error("no compatible host-visible Vulkan memory type found");
}

bool memory_type_is_coherent(VkPhysicalDevice physical_device, std::uint32_t memory_type_index)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    return (memory_properties.memoryTypes[memory_type_index].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

#endif

}  // namespace

VulkanComputeSmokeResult run_vulkan_compute_smoke(
    const std::string& spirv_path,
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_VULKAN
    if (!vulkan_support_built()) {
        throw std::runtime_error("Vulkan support was not built");
    }

    const auto spirv = read_spirv_file(spirv_path);

    const VkInstance instance = create_instance();
    const auto destroy_instance = scope_exit([&] {
        vkDestroyInstance(instance, nullptr);
    });

    const auto selected = select_physical_device(instance, requested_device_index);
    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_create_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = selected.queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo device_create_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
        .pEnabledFeatures = nullptr,
    };

    VkDevice device = VK_NULL_HANDLE;
    require_vk(vkCreateDevice(selected.physical_device, &device_create_info, nullptr, &device),
        "vkCreateDevice");
    const auto destroy_device = scope_exit([&] {
        vkDestroyDevice(device, nullptr);
    });

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, selected.queue_family_index, 0, &queue);

    const VkShaderModuleCreateInfo shader_module_info {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = spirv.size() * sizeof(std::uint32_t),
        .pCode = spirv.data(),
    };
    VkShaderModule shader_module = VK_NULL_HANDLE;
    require_vk(vkCreateShaderModule(device, &shader_module_info, nullptr, &shader_module),
        "vkCreateShaderModule");
    const auto destroy_shader_module = scope_exit([&] {
        vkDestroyShaderModule(device, shader_module, nullptr);
    });

    const VkDescriptorSetLayoutBinding layout_binding {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    const VkDescriptorSetLayoutCreateInfo descriptor_layout_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &layout_binding,
    };
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    require_vk(vkCreateDescriptorSetLayout(
                   device, &descriptor_layout_info, nullptr, &descriptor_set_layout),
        "vkCreateDescriptorSetLayout");
    const auto destroy_descriptor_set_layout = scope_exit([&] {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    });

    const VkPipelineLayoutCreateInfo pipeline_layout_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    require_vk(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout),
        "vkCreatePipelineLayout");
    const auto destroy_pipeline_layout = scope_exit([&] {
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    });

    const VkPipelineShaderStageCreateInfo shader_stage_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
    const VkComputePipelineCreateInfo pipeline_info {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = shader_stage_info,
        .layout = pipeline_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    require_vk(vkCreateComputePipelines(
                   device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
        "vkCreateComputePipelines");
    const auto destroy_pipeline = scope_exit([&] {
        vkDestroyPipeline(device, pipeline, nullptr);
    });

    const VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = kSmokeBufferBytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &buffer), "vkCreateBuffer");
    const auto destroy_buffer = scope_exit([&] {
        vkDestroyBuffer(device, buffer, nullptr);
    });

    VkMemoryRequirements memory_requirements {};
    vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
    const auto memory_type_index = find_memory_type(
        selected.physical_device,
        memory_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const bool coherent = memory_type_is_coherent(
        selected.physical_device, memory_type_index);

    const VkMemoryAllocateInfo allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &memory), "vkAllocateMemory");
    const auto free_memory = scope_exit([&] {
        vkFreeMemory(device, memory, nullptr);
    });
    require_vk(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");

    void* mapped = nullptr;
    require_vk(vkMapMemory(device, memory, 0, memory_requirements.size, 0, &mapped),
        "vkMapMemory");
    const auto unmap_memory = scope_exit([&] {
        vkUnmapMemory(device, memory);
    });

    std::vector<std::uint32_t> input_values(kSmokeValueCount);
    for (std::uint32_t i = 0; i < kSmokeValueCount; ++i) {
        input_values[i] = i;
    }
    std::memcpy(mapped, input_values.data(), input_values.size() * sizeof(std::uint32_t));

    const VkMappedMemoryRange mapped_range {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .pNext = nullptr,
        .memory = memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    if (!coherent) {
        require_vk(vkFlushMappedMemoryRanges(device, 1, &mapped_range),
            "vkFlushMappedMemoryRanges");
    }

    const VkDescriptorPoolSize pool_size {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    const VkDescriptorPoolCreateInfo descriptor_pool_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    require_vk(vkCreateDescriptorPool(
                   device, &descriptor_pool_info, nullptr, &descriptor_pool),
        "vkCreateDescriptorPool");
    const auto destroy_descriptor_pool = scope_exit([&] {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    });

    const VkDescriptorSetAllocateInfo descriptor_allocate_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    };
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    require_vk(vkAllocateDescriptorSets(device, &descriptor_allocate_info, &descriptor_set),
        "vkAllocateDescriptorSets");

    const VkDescriptorBufferInfo descriptor_buffer_info {
        .buffer = buffer,
        .offset = 0,
        .range = kSmokeBufferBytes,
    };
    const VkWriteDescriptorSet descriptor_write {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pImageInfo = nullptr,
        .pBufferInfo = &descriptor_buffer_info,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

    const VkCommandPoolCreateInfo command_pool_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = selected.queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    require_vk(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool),
        "vkCreateCommandPool");
    const auto destroy_command_pool = scope_exit([&] {
        vkDestroyCommandPool(device, command_pool, nullptr);
    });

    const VkCommandBufferAllocateInfo command_buffer_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    require_vk(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer),
        "vkAllocateCommandBuffers");

    const VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    require_vk(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout,
        0,
        1,
        &descriptor_set,
        0,
        nullptr);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    require_vk(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

    const VkFenceCreateInfo fence_info {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    VkFence fence = VK_NULL_HANDLE;
    require_vk(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
    const auto destroy_fence = scope_exit([&] {
        vkDestroyFence(device, fence, nullptr);
    });

    const VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    require_vk(vkQueueSubmit(queue, 1, &submit_info, fence), "vkQueueSubmit");
    require_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs),
        "vkWaitForFences");

    if (!coherent) {
        require_vk(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range),
            "vkInvalidateMappedMemoryRanges");
    }

    std::vector<std::uint32_t> output_values(kSmokeValueCount);
    std::memcpy(output_values.data(), mapped, output_values.size() * sizeof(std::uint32_t));

    return VulkanComputeSmokeResult {
        .device_index = selected.index,
        .device_name = selected.name,
        .values = std::move(output_values),
    };
#else
    (void)spirv_path;
    (void)requested_device_index;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

}  // namespace ldcompress
