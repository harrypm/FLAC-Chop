#include "metal_devices.h"

#include <stdexcept>

namespace ldcompress {

bool metal_support_built()
{
    return false;
}

std::vector<MetalDeviceInfo> list_metal_devices()
{
    return {};
}

MetalDeviceInfo select_metal_device(std::optional<std::size_t> requested_index)
{
    (void)requested_index;
    throw std::runtime_error("Metal support was not built");
}

}  // namespace ldcompress
