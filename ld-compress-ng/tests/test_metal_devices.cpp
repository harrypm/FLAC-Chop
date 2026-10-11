#include "metal_devices.h"

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

void test_metal_device_selection()
{
    const auto devices = ldcompress::list_metal_devices();
    if (!ldcompress::metal_support_built()) {
        require(devices.empty(), "Metal-disabled build returned devices");
        require_throws([] {
            (void)ldcompress::select_metal_device(std::nullopt);
        }, "Metal-disabled build selected a default device");
        require_throws([] {
            (void)ldcompress::select_metal_device(0);
        }, "Metal-disabled build selected an explicit device");
        return;
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        require(devices[i].index == i, "Metal device index mismatch");
    }

    if (devices.empty()) {
        require_throws([] {
            (void)ldcompress::select_metal_device(std::nullopt);
        }, "empty Metal device list selected a default device");
        require_throws([] {
            (void)ldcompress::select_metal_device(0);
        }, "empty Metal device list selected an explicit device");
        return;
    }

    bool saw_available = false;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].available) {
            saw_available = true;
            const auto selected = ldcompress::select_metal_device(i);
            require(selected.index == i, "explicit Metal device selection returned wrong device");
        } else {
            require_throws([i] {
                (void)ldcompress::select_metal_device(i);
            }, "unavailable Metal device was selected");
        }
    }

    if (saw_available) {
        const auto selected = ldcompress::select_metal_device(std::nullopt);
        require(selected.index < devices.size(),
            "default Metal device selection returned out-of-range index");
        require(devices[selected.index].available,
            "default Metal device selection returned unavailable device");
    } else {
        require_throws([] {
            (void)ldcompress::select_metal_device(std::nullopt);
        }, "Metal default selection succeeded with no available devices");
    }

    require_throws([&] {
        (void)ldcompress::select_metal_device(devices.size());
    }, "out-of-range Metal device index was selected");
}

}  // namespace

int main()
{
    try {
        test_metal_device_selection();
    } catch (const std::exception& ex) {
        std::cerr << "test_metal_devices: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
