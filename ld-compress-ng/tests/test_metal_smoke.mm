#include "metal_devices.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
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
    if (text.empty()) {
        throw std::runtime_error("empty Metal device index");
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid Metal device index: " + std::string(text));
        }
        value = (value * 10U) + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

struct Options {
    std::optional<std::size_t> device_index;
};

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
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

std::string ns_string_to_string(NSString* value)
{
    if (value == nil) {
        return {};
    }
    const char* text = [value UTF8String];
    return text == nullptr ? std::string {} : std::string(text);
}

std::string ns_error_text(NSError* error)
{
    if (error == nil) {
        return {};
    }
    return ns_string_to_string([error localizedDescription]);
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

id<MTLDevice> select_device(std::optional<std::size_t> requested_index)
{
    NSArray<id<MTLDevice>>* devices = copy_all_devices();
    if ([devices count] == 0) {
        return nil;
    }
    if (requested_index.has_value()) {
        if (*requested_index >= static_cast<std::size_t>([devices count])) {
            throw std::runtime_error("requested Metal device index is out of range");
        }
        return [devices objectAtIndex:*requested_index];
    }
    for (NSUInteger i = 0; i < [devices count]; ++i) {
        id<MTLDevice> device = [devices objectAtIndex:i];
        if (![device isLowPower]) {
            return device;
        }
    }
    return [devices objectAtIndex:0];
}

void run_metal_smoke(const Options& options)
{
    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal smoke skipped: Metal support was not built\n";
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = select_device(options.device_index);
        if (device == nil) {
            std::cout << "Metal smoke skipped: no Metal device\n";
            return;
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, "Metal smoke could not create a command queue");

        NSString* source = @R"metal(
#include <metal_stdlib>
using namespace metal;

kernel void reduce64(device const ulong* input [[buffer(0)]],
                     device ulong* output [[buffer(1)]],
                     uint lane [[thread_index_in_threadgroup]])
{
    threadgroup ulong scratch[64];
    scratch[lane] = input[lane];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = 32; stride > 0; stride >>= 1) {
        if (lane < stride) {
            scratch[lane] += scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0) {
        output[0] = scratch[0];
    }
}
)metal";

        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (library == nil) {
            throw std::runtime_error("Metal smoke library compilation failed: " +
                ns_error_text(error));
        }
        id<MTLFunction> function = [library newFunctionWithName:@"reduce64"];
        require(function != nil, "Metal smoke could not create reduce64 function");
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (pipeline == nil) {
            throw std::runtime_error("Metal smoke pipeline creation failed: " +
                ns_error_text(error));
        }

        std::array<std::uint64_t, 64> input {};
        std::uint64_t expected = 0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::uint64_t>(i + 1U);
            expected += input[i];
        }
        std::uint64_t output = 0;
        id<MTLBuffer> input_buffer = [device newBufferWithBytes:input.data()
            length:sizeof(input) options:MTLResourceStorageModeShared];
        id<MTLBuffer> output_buffer = [device newBufferWithBytes:&output
            length:sizeof(output) options:MTLResourceStorageModeShared];
        require(input_buffer != nil, "Metal smoke could not allocate input buffer");
        require(output_buffer != nil, "Metal smoke could not allocate output buffer");

        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        require(command_buffer != nil, "Metal smoke could not create command buffer");
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        require(encoder != nil, "Metal smoke could not create compute encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input_buffer offset:0 atIndex:0];
        [encoder setBuffer:output_buffer offset:0 atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if ([command_buffer status] != MTLCommandBufferStatusCompleted) {
            throw std::runtime_error("Metal smoke command buffer did not complete");
        }

        const auto* result =
            static_cast<const std::uint64_t*>([output_buffer contents]);
        require(result[0] == expected, "Metal smoke reduction result mismatch");
        std::cout << "Metal smoke ran on " << ns_string_to_string([device name]) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_args(argc, argv);
        run_metal_smoke(options);
    } catch (const std::exception& ex) {
        std::cerr << "test_metal_smoke: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
