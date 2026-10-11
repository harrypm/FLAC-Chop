#include "vulkan_devices.h"
#include "vulkan_smoke.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t parse_device_index(std::string_view text)
{
    std::size_t value = 0;
    if (text.empty()) {
        throw std::runtime_error("empty Vulkan device index");
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid Vulkan device index: " + std::string(text));
        }
        value = (value * 10U) + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

struct Options {
    std::string spirv_path;
    std::optional<std::size_t> device_index;
};

Options parse_args(int argc, char** argv)
{
    Options options;
    if (!ldcompress::vulkan_support_built()) {
        return options;
    }
    if (argc < 2) {
        throw std::runtime_error("expected path to Vulkan smoke SPIR-V shader");
    }
    options.spirv_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error("--device requires a value");
            }
            options.device_index = parse_device_index(argv[i]);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    return options;
}

void test_vulkan_smoke(const Options& options)
{
    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan smoke skipped: Vulkan support was not built\n";
        return;
    }

    const auto devices = ldcompress::list_vulkan_devices();
    if (devices.empty()) {
        std::cout << "Vulkan smoke skipped: no Vulkan devices found\n";
        return;
    }

    bool has_compute_device = false;
    for (const auto& device : devices) {
        has_compute_device = has_compute_device || device.available;
    }
    if (!has_compute_device) {
        std::cout << "Vulkan smoke skipped: no Vulkan compute devices found\n";
        return;
    }

    const auto result =
        ldcompress::run_vulkan_compute_smoke(options.spirv_path, options.device_index);
    require(!result.device_name.empty(), "Vulkan smoke did not report a device name");
    require(result.values.size() == 64, "Vulkan smoke result size mismatch");
    for (std::size_t i = 0; i < result.values.size(); ++i) {
        const auto expected = static_cast<std::uint32_t>(i * 3U + 7U);
        if (result.values[i] != expected) {
            throw std::runtime_error(
                "Vulkan smoke value mismatch at " + std::to_string(i) +
                ": expected " + std::to_string(expected) +
                ", got " + std::to_string(result.values[i]));
        }
    }

    std::cout << "Vulkan smoke ran on [" << result.device_index << "] "
              << result.device_name << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        test_vulkan_smoke(parse_args(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << "test_vulkan_smoke: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
