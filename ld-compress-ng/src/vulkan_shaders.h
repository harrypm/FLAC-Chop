#pragma once

#include <cstddef>
#include <cstdint>

namespace ldcompress::vulkan_detail {

struct VulkanShaderBytes {
    const std::uint32_t* data = nullptr;
    std::size_t size_bytes = 0;

    std::size_t word_count() const
    {
        return size_bytes / sizeof(std::uint32_t);
    }
};

VulkanShaderBytes fixed_constant_shader_spirv();
VulkanShaderBytes generated_shader_spirv();

}  // namespace ldcompress::vulkan_detail
