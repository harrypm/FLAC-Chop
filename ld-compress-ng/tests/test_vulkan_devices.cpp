#include "vulkan_devices.h"

#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const char* message)
{
    try {
        fn();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

void require_out_of_range_message(std::size_t requested_index, std::size_t visible_devices)
{
    try {
        (void)ldcompress::select_vulkan_device(requested_index);
    } catch (const std::runtime_error& ex) {
        const std::string text = ex.what();
        require(text.find("Vulkan device index out of range: " +
                    std::to_string(requested_index)) != std::string::npos,
            "out-of-range Vulkan device error did not include requested index");
        require(text.find("visible devices: " + std::to_string(visible_devices)) !=
                std::string::npos,
            "out-of-range Vulkan device error did not include visible device count");
        require(text.find("ld-compress-ng devices") != std::string::npos,
            "out-of-range Vulkan device error did not point to devices command");
        return;
    }
    throw std::runtime_error("out-of-range Vulkan device index was selected");
}

void test_vulkan_device_selection()
{
    const auto devices = ldcompress::list_vulkan_devices();
    if (!ldcompress::vulkan_support_built()) {
        require(devices.empty(), "Vulkan-disabled build returned devices");
        require_throws([] {
            (void)ldcompress::select_vulkan_device(std::nullopt);
        }, "Vulkan-disabled build selected a default device");
        require_throws([] {
            (void)ldcompress::select_vulkan_device(0);
        }, "Vulkan-disabled build selected an explicit device");
        return;
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        require(devices[i].index == i, "Vulkan device index mismatch");
    }

    if (devices.empty()) {
        require_throws([] {
            (void)ldcompress::select_vulkan_device(std::nullopt);
        }, "empty Vulkan device list selected a default device");
        require_throws([] {
            (void)ldcompress::select_vulkan_device(0);
        }, "empty Vulkan device list selected an explicit device");
        return;
    }

    bool saw_available = false;
    std::size_t first_available = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].available) {
            require(devices[i].compute_queue_count > 0,
                "available Vulkan device reported no compute queues");
            if (!saw_available) {
                first_available = i;
                saw_available = true;
            }
            const auto selected = ldcompress::select_vulkan_device(i);
            require(selected.index == i, "explicit Vulkan device selection returned wrong device");
        } else {
            require_throws([i] {
                (void)ldcompress::select_vulkan_device(i);
            }, "unavailable Vulkan device was selected");
        }
    }

    if (saw_available) {
        const auto selected = ldcompress::select_vulkan_device(std::nullopt);
        require(selected.index == first_available,
            "default Vulkan device selection did not use first available device");
    } else {
        require_throws([] {
            (void)ldcompress::select_vulkan_device(std::nullopt);
        }, "Vulkan default selection succeeded with no available devices");
    }

    require_out_of_range_message(devices.size(), devices.size());
}

}  // namespace

int main()
{
    try {
        test_vulkan_device_selection();
    } catch (const std::exception& ex) {
        std::cerr << "test_vulkan_devices: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
