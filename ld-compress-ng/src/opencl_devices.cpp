#include "opencl_devices.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef LDCOMPRESS_HAVE_OPENCL
#define LDCOMPRESS_HAVE_OPENCL 0
#endif

#if LDCOMPRESS_HAVE_OPENCL
#if defined(__APPLE__)
#include <OpenCL/opencl.h>
#else
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>
#endif

#include <vector>
#endif

namespace ldcompress {
namespace {

#if LDCOMPRESS_HAVE_OPENCL

constexpr cl_int kPlatformNotFound = -1001;

std::string cl_error_name(cl_int status)
{
    switch (status) {
    case CL_SUCCESS:
        return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND:
        return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE:
        return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE:
        return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
        return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
        return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
        return "CL_OUT_OF_HOST_MEMORY";
    case CL_PROFILING_INFO_NOT_AVAILABLE:
        return "CL_PROFILING_INFO_NOT_AVAILABLE";
    case CL_MEM_COPY_OVERLAP:
        return "CL_MEM_COPY_OVERLAP";
    case CL_IMAGE_FORMAT_MISMATCH:
        return "CL_IMAGE_FORMAT_MISMATCH";
    case CL_IMAGE_FORMAT_NOT_SUPPORTED:
        return "CL_IMAGE_FORMAT_NOT_SUPPORTED";
    case CL_BUILD_PROGRAM_FAILURE:
        return "CL_BUILD_PROGRAM_FAILURE";
    case CL_MAP_FAILURE:
        return "CL_MAP_FAILURE";
    case CL_INVALID_VALUE:
        return "CL_INVALID_VALUE";
    case CL_INVALID_DEVICE_TYPE:
        return "CL_INVALID_DEVICE_TYPE";
    case CL_INVALID_PLATFORM:
        return "CL_INVALID_PLATFORM";
    case CL_INVALID_DEVICE:
        return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT:
        return "CL_INVALID_CONTEXT";
    case CL_INVALID_QUEUE_PROPERTIES:
        return "CL_INVALID_QUEUE_PROPERTIES";
    case CL_INVALID_COMMAND_QUEUE:
        return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_HOST_PTR:
        return "CL_INVALID_HOST_PTR";
    case CL_INVALID_MEM_OBJECT:
        return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR:
        return "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
    case CL_INVALID_IMAGE_SIZE:
        return "CL_INVALID_IMAGE_SIZE";
    case CL_INVALID_SAMPLER:
        return "CL_INVALID_SAMPLER";
    case CL_INVALID_BINARY:
        return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS:
        return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM:
        return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE:
        return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME:
        return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL_DEFINITION:
        return "CL_INVALID_KERNEL_DEFINITION";
    case CL_INVALID_KERNEL:
        return "CL_INVALID_KERNEL";
    case CL_INVALID_ARG_INDEX:
        return "CL_INVALID_ARG_INDEX";
    case CL_INVALID_ARG_VALUE:
        return "CL_INVALID_ARG_VALUE";
    case CL_INVALID_ARG_SIZE:
        return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_KERNEL_ARGS:
        return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_DIMENSION:
        return "CL_INVALID_WORK_DIMENSION";
    case CL_INVALID_WORK_GROUP_SIZE:
        return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_WORK_ITEM_SIZE:
        return "CL_INVALID_WORK_ITEM_SIZE";
    case CL_INVALID_GLOBAL_OFFSET:
        return "CL_INVALID_GLOBAL_OFFSET";
    case CL_INVALID_EVENT_WAIT_LIST:
        return "CL_INVALID_EVENT_WAIT_LIST";
    case CL_INVALID_EVENT:
        return "CL_INVALID_EVENT";
    case CL_INVALID_OPERATION:
        return "CL_INVALID_OPERATION";
    case CL_INVALID_GL_OBJECT:
        return "CL_INVALID_GL_OBJECT";
    case CL_INVALID_BUFFER_SIZE:
        return "CL_INVALID_BUFFER_SIZE";
    case CL_INVALID_MIP_LEVEL:
        return "CL_INVALID_MIP_LEVEL";
    default:
        if (status == kPlatformNotFound) {
            return "CL_PLATFORM_NOT_FOUND_KHR";
        }
        return "OpenCL error " + std::to_string(status);
    }
}

void require_cl(cl_int status, const char* operation)
{
    if (status != CL_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + cl_error_name(status));
    }
}

std::string trim_trailing_nul(std::string value)
{
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string get_platform_string(cl_platform_id platform, cl_platform_info param)
{
    std::size_t size = 0;
    require_cl(clGetPlatformInfo(platform, param, 0, nullptr, &size), "clGetPlatformInfo");
    std::string value(size, '\0');
    require_cl(clGetPlatformInfo(platform, param, value.size(), value.data(), nullptr),
        "clGetPlatformInfo");
    return trim_trailing_nul(std::move(value));
}

std::string get_device_string(cl_device_id device, cl_device_info param)
{
    std::size_t size = 0;
    require_cl(clGetDeviceInfo(device, param, 0, nullptr, &size), "clGetDeviceInfo");
    std::string value(size, '\0');
    require_cl(clGetDeviceInfo(device, param, value.size(), value.data(), nullptr),
        "clGetDeviceInfo");
    return trim_trailing_nul(std::move(value));
}

template <typename T>
T get_device_value(cl_device_id device, cl_device_info param)
{
    T value {};
    require_cl(clGetDeviceInfo(device, param, sizeof(value), &value, nullptr), "clGetDeviceInfo");
    return value;
}

std::string device_type_name(cl_device_type type)
{
    std::vector<std::string> names;
    if ((type & CL_DEVICE_TYPE_DEFAULT) != 0) {
        names.emplace_back("default");
    }
    if ((type & CL_DEVICE_TYPE_CPU) != 0) {
        names.emplace_back("cpu");
    }
    if ((type & CL_DEVICE_TYPE_GPU) != 0) {
        names.emplace_back("gpu");
    }
    if ((type & CL_DEVICE_TYPE_ACCELERATOR) != 0) {
        names.emplace_back("accelerator");
    }
#ifdef CL_DEVICE_TYPE_CUSTOM
    if ((type & CL_DEVICE_TYPE_CUSTOM) != 0) {
        names.emplace_back("custom");
    }
#endif

    if (names.empty()) {
        return "unknown";
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            out << '+';
        }
        out << names[i];
    }
    return out.str();
}

#endif

}  // namespace

bool opencl_support_built()
{
    return LDCOMPRESS_HAVE_OPENCL != 0;
}

std::vector<OpenClDeviceInfo> list_opencl_devices()
{
#if LDCOMPRESS_HAVE_OPENCL
    cl_uint platform_count = 0;
    const cl_int platform_status = clGetPlatformIDs(0, nullptr, &platform_count);
    if (platform_status == kPlatformNotFound || platform_count == 0) {
        return {};
    }
    require_cl(platform_status, "clGetPlatformIDs");

    std::vector<cl_platform_id> platforms(platform_count);
    require_cl(clGetPlatformIDs(platform_count, platforms.data(), nullptr), "clGetPlatformIDs");

    std::vector<OpenClDeviceInfo> devices;
    for (std::size_t platform_index = 0; platform_index < platforms.size(); ++platform_index) {
        const auto platform = platforms[platform_index];
        cl_uint device_count = 0;
        const cl_int device_count_status =
            clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
        if (device_count_status == CL_DEVICE_NOT_FOUND || device_count == 0) {
            continue;
        }
        require_cl(device_count_status, "clGetDeviceIDs");

        std::vector<cl_device_id> platform_devices(device_count);
        require_cl(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count,
                       platform_devices.data(), nullptr),
            "clGetDeviceIDs");

        for (std::size_t device_index = 0; device_index < platform_devices.size(); ++device_index) {
            const auto device = platform_devices[device_index];
            const auto type = get_device_value<cl_device_type>(device, CL_DEVICE_TYPE);
            OpenClDeviceInfo info;
            info.flat_index = static_cast<std::uint32_t>(devices.size());
            info.platform_index = static_cast<std::uint32_t>(platform_index);
            info.device_index = static_cast<std::uint32_t>(device_index);
            info.platform_name = get_platform_string(platform, CL_PLATFORM_NAME);
            info.platform_vendor = get_platform_string(platform, CL_PLATFORM_VENDOR);
            info.platform_version = get_platform_string(platform, CL_PLATFORM_VERSION);
            info.device_name = get_device_string(device, CL_DEVICE_NAME);
            info.device_vendor = get_device_string(device, CL_DEVICE_VENDOR);
            info.device_version = get_device_string(device, CL_DEVICE_VERSION);
            info.driver_version = get_device_string(device, CL_DRIVER_VERSION);
            info.type = device_type_name(type);
            info.compute_units =
                get_device_value<cl_uint>(device, CL_DEVICE_MAX_COMPUTE_UNITS);
            info.global_memory_bytes =
                get_device_value<cl_ulong>(device, CL_DEVICE_GLOBAL_MEM_SIZE);
            info.hardware_accelerator =
                (type & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR)) != 0;
            info.available = get_device_value<cl_bool>(device, CL_DEVICE_AVAILABLE) == CL_TRUE;
            devices.push_back(std::move(info));
        }
    }

    return devices;
#else
    return {};
#endif
}

OpenClDeviceInfo select_opencl_device(std::optional<std::size_t> requested_index)
{
    if (!opencl_support_built()) {
        throw std::runtime_error("OpenCL support was not built");
    }

    const auto devices = list_opencl_devices();
    if (devices.empty()) {
        throw std::runtime_error("no OpenCL devices found");
    }

    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("OpenCL device index out of range: " +
                std::to_string(*requested_index));
        }
        if (!devices[*requested_index].available) {
            throw std::runtime_error("selected OpenCL device is not available: " +
                devices[*requested_index].device_name);
        }
        return devices[*requested_index];
    }

    for (const auto& device : devices) {
        if (device.available) {
            return device;
        }
    }

    throw std::runtime_error("no available OpenCL devices found");
}

bool opencl_device_is_auto_eligible(const OpenClDeviceInfo& device) noexcept
{
    return device.available && device.hardware_accelerator;
}

}  // namespace ldcompress
