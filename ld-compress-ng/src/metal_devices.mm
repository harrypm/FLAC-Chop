#include "metal_devices.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ldcompress {
namespace {

std::string ns_string_to_string(NSString* value)
{
    if (value == nil) {
        return {};
    }
    const char* text = [value UTF8String];
    return text == nullptr ? std::string {} : std::string(text);
}

MetalDeviceInfo make_device_info(id<MTLDevice> device, std::uint32_t index)
{
    MetalDeviceInfo info;
    info.index = index;
    info.device_name = ns_string_to_string([device name]);
    info.available = true;
    info.low_power = [device isLowPower];
    info.removable = [device isRemovable];
    if ([device respondsToSelector:@selector(hasUnifiedMemory)]) {
        info.unified_memory = [device hasUnifiedMemory];
    }
    if ([device respondsToSelector:@selector(recommendedMaxWorkingSetSize)]) {
        info.recommended_max_working_set_bytes =
            static_cast<std::uint64_t>([device recommendedMaxWorkingSetSize]);
    }
    info.registry_id = static_cast<std::uint64_t>([device registryID]);
    info.max_buffer_length = static_cast<std::uint64_t>([device maxBufferLength]);
    info.max_threads_per_threadgroup = 0;
    return info;
}

NSArray<id<MTLDevice>>* copy_all_devices()
{
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    if (devices != nil && [devices count] != 0) {
        return devices;
    }

    id<MTLDevice> default_device = MTLCreateSystemDefaultDevice();
    if (default_device == nil) {
        return @[];
    }
    return @[ default_device ];
}

}  // namespace

bool metal_support_built()
{
    return true;
}

std::vector<MetalDeviceInfo> list_metal_devices()
{
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = copy_all_devices();
        std::vector<MetalDeviceInfo> result;
        result.reserve(static_cast<std::size_t>([devices count]));
        for (NSUInteger i = 0; i < [devices count]; ++i) {
            result.push_back(make_device_info(
                [devices objectAtIndex:i], static_cast<std::uint32_t>(i)));
        }
        return result;
    }
}

MetalDeviceInfo select_metal_device(std::optional<std::size_t> requested_index)
{
    const auto devices = list_metal_devices();
    if (devices.empty()) {
        throw std::runtime_error("no Metal devices found");
    }

    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            std::ostringstream out;
            out << "Metal device index out of range: " << *requested_index
                << " (visible devices: " << devices.size()
                << "; run ld-compress-ng devices)";
            throw std::runtime_error(out.str());
        }
        if (!devices[*requested_index].available) {
            throw std::runtime_error("selected Metal device is unavailable: " +
                devices[*requested_index].device_name);
        }
        return devices[*requested_index];
    }

    for (const auto& device : devices) {
        if (device.available && !device.low_power) {
            return device;
        }
    }
    for (const auto& device : devices) {
        if (device.available) {
            return device;
        }
    }

    throw std::runtime_error("no available Metal devices found");
}

}  // namespace ldcompress
