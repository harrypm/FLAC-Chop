#include "metal_analysis.h"

#include "flac_native_writer.h"
#include "metal_devices.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ldcompress::metal_detail {
namespace {

using opencl_detail::FlacClRiceParameterSet;
using opencl_detail::FlacClSubframeTask;
using opencl_detail::OpenClMonoAnalysisTaskPlan;
using opencl_detail::OpenClMonoBestMethodResult;
using opencl_detail::OpenClMonoFixedConstantAnalysisResult;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kWorkgroupSize = 64;
constexpr std::size_t kMetalMaxBlockSize = 8192;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kGeneratedTukeyTaperFraction = 0.5;
constexpr double kSubdivideTukey3TaperFraction = 0.5 / 3.0;
constexpr std::size_t kSubdivideTukey3WindowCount = 9;
constexpr unsigned kMetalExactLeafMaxRicePartitionOrder = 6;

struct ExactParams {
    std::uint32_t selected_task_count = 0;
    std::uint32_t max_rice_partition_order = 0;
    std::uint32_t analysis_profile = 0;
    std::uint32_t prepared_frame_facts = 0;
};

struct ChooseParams {
    std::uint32_t frame_count = 0;
    std::uint32_t tasks_per_frame = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct GeneratedLpcParams {
    std::uint32_t frame_count = 0;
    std::uint32_t tasks_per_frame = 0;
    std::uint32_t lpc_tasks_per_window = 0;
    std::uint32_t total_lpc_tasks = 0;
    std::uint32_t window_count = 0;
    std::uint32_t max_lpc_order = 0;
    std::uint32_t coefficient_precision = 0;
    std::uint32_t analysis_profile = 0;
    std::uint32_t blocksize = 0;
    std::uint32_t fixed_order_guess_on_gpu = 0;
    std::uint32_t shifted_samples_ready = 0;
    std::uint32_t reserved1 = 0;
};

struct GeneratedLpcConfig {
    std::size_t frame_count = 0;
    std::size_t tasks_per_frame = 0;
    std::size_t lpc_tasks_per_window = 0;
    std::size_t total_lpc_tasks = 0;
    std::size_t window_count = 0;
    std::size_t blocksize = 0;
    unsigned max_lpc_order = 0;
};

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point finish)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += elapsed_ns(start, Clock::now());
}

std::size_t checked_buffer_bytes(std::size_t count, std::size_t element_size, const char* name)
{
    if (count == 0) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " buffer is empty");
    }
    if (count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " buffer is too large");
    }
    return count * element_size;
}

std::uint32_t checked_u32(std::size_t value, const char* name)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("Metal analysis ") + name + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
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
    NSString* text = [error localizedDescription];
    return ns_string_to_string(text);
}

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("Metal mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("Metal mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("Metal mono analysis currently supports full mono selection only");
    }
    if ((plan.selected_tasks.size() % plan.estimate_tasks_per_frame) != 0 ||
        (plan.residual_tasks.size() % plan.residual_tasks_per_frame) != 0) {
        throw std::runtime_error("Metal mono analysis task plan is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("Metal mono analysis selected/residual frame counts differ");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 || static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("Metal mono analysis selected task index is out of range");
        }
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        if (first_task.data.blocksize <= 0 ||
            static_cast<std::size_t>(first_task.data.blocksize) > kMetalMaxBlockSize) {
            throw std::runtime_error("Metal mono analysis block size is unsupported");
        }
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("Metal mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base || selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("Metal mono analysis selected task crosses frame group");
            }
        }
    }
}

std::size_t mono_plan_frame_count(const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    return plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
}

std::uint32_t metal_analysis_profile_arg(NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return 0;
    case NativeAnalysisProfile::OrderGuessExactRice:
        return 1;
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return 2;
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return 3;
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        return 4;
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        return 5;
    }
    return 0;
}

bool generated_profile_uses_subdivide_tukey3(NativeAnalysisProfile profile)
{
    return profile == NativeAnalysisProfile::SubdivideTukey3MeanRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
}

float tukey_weight(std::size_t n, std::size_t blocksize, double taper_fraction)
{
    if (blocksize <= 1 || taper_fraction <= 0.0) {
        return 1.0F;
    }
    if (taper_fraction >= 1.0) {
        taper_fraction = 1.0;
    }
    const auto edge_width = static_cast<std::size_t>(
        (taper_fraction / 2.0) * static_cast<double>(blocksize - 1U));
    if (edge_width == 0) {
        return 1.0F;
    }
    if (n < edge_width) {
        return static_cast<float>(
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n) /
                static_cast<double>(edge_width))));
    }
    if (n > blocksize - edge_width - 1U) {
        return static_cast<float>(
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(blocksize - n - 1U) /
                static_cast<double>(edge_width))));
    }
    return 1.0F;
}

float partial_tukey_weight(
    std::size_t n,
    std::size_t blocksize,
    double taper_fraction,
    double start,
    double end)
{
    if (blocksize == 0 || end <= start) {
        return 0.0F;
    }

    const auto start_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(start * static_cast<double>(blocksize)));
    const auto end_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(end * static_cast<double>(blocksize)));
    const auto width = end_n > start_n ? end_n - start_n : 0;
    if (width == 0 || n < start_n || n >= end_n) {
        return 0.0F;
    }

    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto edge_width = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(width));
    double weight = 1.0;
    if (edge_width != 0 && n < start_n + edge_width) {
        const auto i = n - start_n + 1U;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(edge_width)));
    } else if (edge_width != 0 && n >= end_n - edge_width) {
        const auto i = end_n - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(edge_width)));
    }
    return static_cast<float>(weight);
}

float punchout_tukey_weight(
    std::size_t n,
    std::size_t blocksize,
    double taper_fraction,
    double start,
    double end)
{
    if (blocksize == 0 || end <= start) {
        return 1.0F;
    }

    const auto start_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(start * static_cast<double>(blocksize)));
    const auto end_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(end * static_cast<double>(blocksize)));
    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto left_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(start_n));
    const auto right_span = blocksize > end_n ? blocksize - end_n : 0;
    const auto right_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(right_span));

    double weight = 1.0;
    if (n >= start_n && n < end_n) {
        weight = 0.0;
    } else if (left_edge != 0 && n < left_edge) {
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(n + 1U) /
                static_cast<double>(left_edge)));
    } else if (left_edge != 0 && n >= start_n - left_edge && n < start_n) {
        const auto i = start_n - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(left_edge)));
    } else if (right_edge != 0 && n >= end_n && n < end_n + right_edge) {
        const auto i = n - end_n + 1U;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(right_edge)));
    } else if (right_edge != 0 && n >= blocksize - right_edge) {
        const auto i = blocksize - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(right_edge)));
    }
    return static_cast<float>(weight);
}

std::vector<float> make_generated_lpc_windows(
    std::size_t blocksize,
    std::size_t window_count,
    NativeAnalysisProfile profile)
{
    std::vector<float> windows(blocksize * window_count, 1.0F);
    if (generated_profile_uses_subdivide_tukey3(profile)) {
        if (blocksize == 0) {
            return windows;
        }
        if (window_count != kSubdivideTukey3WindowCount) {
            throw std::runtime_error("Metal subdivide-Tukey3 analysis requires nine LPC windows");
        }

        for (std::size_t n = 0; n < blocksize; ++n) {
            windows[n] = tukey_weight(n, blocksize, kSubdivideTukey3TaperFraction);
        }

        std::size_t window = 1;
        for (unsigned parts : {2U, 3U}) {
            for (unsigned part = 0; part < parts; ++part, ++window) {
                const auto start = static_cast<double>(part) / static_cast<double>(parts);
                const auto end = static_cast<double>(part + 1U) / static_cast<double>(parts);
                auto* values = windows.data() + (window * blocksize);
                for (std::size_t n = 0; n < blocksize; ++n) {
                    values[n] = partial_tukey_weight(
                        n, blocksize, kSubdivideTukey3TaperFraction, start, end);
                }
            }
        }
        for (unsigned part = 0; part < 3; ++part, ++window) {
            const auto start = static_cast<double>(part) / 3.0;
            const auto end = static_cast<double>(part + 1U) / 3.0;
            auto* values = windows.data() + (window * blocksize);
            for (std::size_t n = 0; n < blocksize; ++n) {
                values[n] = punchout_tukey_weight(
                    n, blocksize, kSubdivideTukey3TaperFraction, start, end);
            }
        }
        if (window != window_count) {
            throw std::runtime_error("Metal subdivide-Tukey3 LPC window count mismatch");
        }
        return windows;
    }

    if (window_count < 2 || blocksize <= 1) {
        return windows;
    }

    auto* tukey = windows.data() + blocksize;
    for (std::size_t n = 0; n < blocksize; ++n) {
        tukey[n] = tukey_weight(n, blocksize, kGeneratedTukeyTaperFraction);
    }
    if (window_count < 3) {
        return windows;
    }

    auto* welch = windows.data() + (2U * blocksize);
    const auto endpoint = static_cast<double>(blocksize - 1U);
    const auto midpoint = endpoint / 2.0;
    for (std::size_t n = 0; n < blocksize; ++n) {
        const auto k = (static_cast<double>(n) - midpoint) / midpoint;
        welch[n] = static_cast<float>(1.0 - (k * k));
    }
    return windows;
}

GeneratedLpcConfig generated_lpc_config(const OpenClMonoAnalysisTaskPlan& plan)
{
    const auto frame_count = mono_plan_frame_count(plan);
    std::size_t total_lpc_tasks = 0;
    while (total_lpc_tasks < plan.residual_tasks_per_frame &&
        plan.residual_tasks[total_lpc_tasks].data.type ==
            opencl_detail::kFlacClSubframeLpc) {
        ++total_lpc_tasks;
    }
    if (plan.max_lpc_order == 0 || total_lpc_tasks == 0) {
        throw std::runtime_error("Metal generated LPC analysis requires LPC tasks");
    }

    std::size_t lpc_tasks_per_window = 0;
    while (lpc_tasks_per_window < total_lpc_tasks &&
        plan.residual_tasks[lpc_tasks_per_window].data.residualOrder ==
            static_cast<std::int32_t>(lpc_tasks_per_window + 1U)) {
        ++lpc_tasks_per_window;
    }
    if (lpc_tasks_per_window == 0) {
        lpc_tasks_per_window = 1;
    }
    if ((total_lpc_tasks + lpc_tasks_per_window - 1U) / lpc_tasks_per_window >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Metal generated LPC window count exceeds uint32");
    }
    const auto window_count =
        (total_lpc_tasks + lpc_tasks_per_window - 1U) / lpc_tasks_per_window;

    const auto blocksize = static_cast<std::size_t>(plan.residual_tasks.front().data.blocksize);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_base = frame * plan.residual_tasks_per_frame;
        for (std::size_t slot = 0; slot < total_lpc_tasks; ++slot) {
            const auto& task = plan.residual_tasks.at(frame_base + slot);
            if (task.data.type != opencl_detail::kFlacClSubframeLpc) {
                throw std::runtime_error("Metal generated LPC task prefix is not contiguous");
            }
            if (task.data.blocksize <= 0 ||
                static_cast<std::size_t>(task.data.blocksize) != blocksize) {
                throw std::runtime_error("Metal generated LPC task block sizes differ");
            }
            if (task.data.obits != 16) {
                throw std::runtime_error("Metal generated LPC currently supports 16-bit tasks");
            }
        }
    }

    return GeneratedLpcConfig {
        .frame_count = frame_count,
        .tasks_per_frame = plan.residual_tasks_per_frame,
        .lpc_tasks_per_window = lpc_tasks_per_window,
        .total_lpc_tasks = total_lpc_tasks,
        .window_count = window_count,
        .blocksize = blocksize,
        .max_lpc_order = plan.max_lpc_order,
    };
}

const char* metal_analysis_source()
{
    return R"MSL(
/*
 * Portions adapted from CUETools.FLACCL-compatible mono analysis kernels.
 * Copyright (c) 2010-2022 Gregory S. Chudov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Local modifications for ld-compress-ng, 2026-07-08:
 * - Ported exact mono task costing and best-task sidecar output to Metal.
 */
#include <metal_stdlib>
using namespace metal;

constant uint kWorkgroupSize = 64u;
constant uint kMaxOrder = 32u;
constant uint kMaxRiceParameter = 14u;
constant uint kMaxRicePartitionCount = 256u;
constant uint kExactLeafMaxRicePartitionOrder = 6u;
constant uint kExactLeafRiceParameterCount = 15u;
constant int kSubframeConstant = 0;
constant int kSubframeFixed = 8;
constant int kSubframeLpc = 32;
constant int kIntMax = 2147483647;

struct FlacClSubframeData {
    int residualOrder;
    int samplesOffs;
    int shift;
    int cbits;
    int size;
    int type;
    int obits;
    int blocksize;
    int coding_method;
    int channel;
    int residualOffs;
    int wbits;
    int abits;
    int porder;
    int headerLen;
    int encodingOffset;
};

struct FlacClSubframeTask {
    FlacClSubframeData data;
    int coefs[32];
};

struct ExactParams {
    uint selectedTaskCount;
    uint maxRicePartitionOrder;
    uint analysisProfile;
    uint preparedFrameFacts;
};

struct ChooseParams {
    uint frameCount;
    uint tasksPerFrame;
    uint reserved0;
    uint reserved1;
};

struct GeneratedLpcParams {
    uint frameCount;
    uint tasksPerFrame;
    uint lpcTasksPerWindow;
    uint totalLpcTasks;
    uint windowCount;
    uint maxLpcOrder;
    uint coefficientPrecision;
    uint analysisProfile;
    uint blocksize;
    uint fixedOrderGuessOnGpu;
    uint shiftedSamplesReady;
    uint reserved1;
};

struct RiceChoice {
    ulong bits;
    uint parameter;
};

struct MeanRiceChoice {
    ulong exactBits;
    ulong estimatedBits;
    uint parameter;
};

uint bit_width_u32(uint value)
{
    uint bits = 0u;
    while (value != 0u) {
        value >>= 1u;
        ++bits;
    }
    return bits;
}

int sample_trailing_zero_bits(int smp, int bits_per_sample)
{
    if (smp == 0) {
        return bits_per_sample;
    }
    const uint mask = bits_per_sample >= 32
        ? 0xffffffffu
        : ((1u << uint(bits_per_sample)) - 1u);
    uint raw = uint(smp) & mask;
    int zeros = 0;
    while (zeros < bits_per_sample && (raw & 1u) == 0u) {
        raw >>= 1u;
        ++zeros;
    }
    return zeros;
}

int reduce_min_i32(int value, threadgroup int* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] = min(scratch[lane], scratch[lane + stride]);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

uint reduce_or_u32(uint value, threadgroup uint* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] |= scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

ulong reduce_sum_u64(ulong value, threadgroup ulong* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] += scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

int common_wasted_bits(device const int* data, int blocksize, int obits,
    threadgroup int* scratch, uint lane)
{
    int wasted = obits;
    for (int i = int(lane); i < blocksize; i += int(kWorkgroupSize)) {
        wasted = min(wasted, sample_trailing_zero_bits(data[i], obits));
    }
    return min(reduce_min_i32(wasted, scratch, lane), obits - 1);
}

int amplitude_bits(device const int* data, int blocksize, int wasted_bits,
    threadgroup uint* scratch, uint lane)
{
    uint amplitude_or = 0u;
    for (int i = int(lane); i < blocksize; i += int(kWorkgroupSize)) {
        const int sample = data[i];
        const uint folded = sample < 0
            ? uint(-(long(sample) + 1L))
            : uint(sample);
        amplitude_or |= folded;
    }
    const uint reduced = reduce_or_u32(amplitude_or, scratch, lane);
    if (reduced == 0u) {
        return 1;
    }
    const uint bits = bit_width_u32(reduced);
    return int(max(1u, bits > uint(wasted_bits) ? bits - uint(wasted_bits) : 0u));
}

int all_samples_equal(device const int* data, int blocksize,
    threadgroup uint* scratch, uint lane)
{
    const int first = data[0];
    uint mismatch = 0u;
    for (int i = int(lane) + 1; i < blocksize; i += int(kWorkgroupSize)) {
        if (data[i] != first) {
            mismatch = 1u;
        }
    }
    return reduce_or_u32(mismatch, scratch, lane) == 0u;
}

float reduce_sum_float(float value, threadgroup float* scratch, uint lane)
{
    scratch[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = kWorkgroupSize >> 1u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane] += scratch[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    return scratch[0];
}

long shifted_sample(device const int* data, int pos, int wbits)
{
    const long sample = long(data[pos]);
    return wbits == 0 ? sample : sample / (1L << wbits);
}

long fixed_residual(device const int* data, int pos, int order, int wbits)
{
    const long sample = shifted_sample(data, pos, wbits);
    switch (order) {
    case 0:
        return sample;
    case 1:
        return sample - shifted_sample(data, pos - 1, wbits);
    case 2:
        return sample - (2L * shifted_sample(data, pos - 1, wbits)) +
            shifted_sample(data, pos - 2, wbits);
    case 3:
        return sample - (3L * shifted_sample(data, pos - 1, wbits)) +
            (3L * shifted_sample(data, pos - 2, wbits)) -
            shifted_sample(data, pos - 3, wbits);
    case 4:
        return sample - (4L * shifted_sample(data, pos - 1, wbits)) +
            (6L * shifted_sample(data, pos - 2, wbits)) -
            (4L * shifted_sample(data, pos - 3, wbits)) +
            shifted_sample(data, pos - 4, wbits);
    default:
        return 0L;
    }
}

long arithmetic_shift_right(long value, int shift)
{
    if (shift == 0) {
        return value;
    }
    if (value >= 0) {
        return value >> shift;
    }
    const long divisor = 1L << shift;
    return -(((-value) + divisor - 1L) >> shift);
}

long lpc_residual(device const int* data, int pos, thread const FlacClSubframeTask& task)
{
    const int order = task.data.residualOrder;
    long sum = 0L;
    for (int i = 0; i < order; ++i) {
        sum += long(task.coefs[i]) *
            shifted_sample(data, pos - order + i, task.data.wbits);
    }
    return shifted_sample(data, pos, task.data.wbits) -
        arithmetic_shift_right(sum, task.data.shift);
}

long residual(device const int* data, int pos, thread const FlacClSubframeTask& task)
{
    if (task.data.type == kSubframeLpc) {
        return lpc_residual(data, pos, task);
    }
    return fixed_residual(data, pos, task.data.residualOrder, task.data.wbits);
}

int lpc_residual_shifted_i32(
    device const int* data, int pos, thread const FlacClSubframeTask& task)
{
    const int order = task.data.residualOrder;
    if (order == 12) {
        int sum = task.coefs[0] * data[pos - 12];
        sum += task.coefs[1] * data[pos - 11];
        sum += task.coefs[2] * data[pos - 10];
        sum += task.coefs[3] * data[pos - 9];
        sum += task.coefs[4] * data[pos - 8];
        sum += task.coefs[5] * data[pos - 7];
        sum += task.coefs[6] * data[pos - 6];
        sum += task.coefs[7] * data[pos - 5];
        sum += task.coefs[8] * data[pos - 4];
        sum += task.coefs[9] * data[pos - 3];
        sum += task.coefs[10] * data[pos - 2];
        sum += task.coefs[11] * data[pos - 1];
        return data[pos] - int(arithmetic_shift_right(long(sum), task.data.shift));
    }
    if (order == 11) {
        int sum = task.coefs[0] * data[pos - 11];
        sum += task.coefs[1] * data[pos - 10];
        sum += task.coefs[2] * data[pos - 9];
        sum += task.coefs[3] * data[pos - 8];
        sum += task.coefs[4] * data[pos - 7];
        sum += task.coefs[5] * data[pos - 6];
        sum += task.coefs[6] * data[pos - 5];
        sum += task.coefs[7] * data[pos - 4];
        sum += task.coefs[8] * data[pos - 3];
        sum += task.coefs[9] * data[pos - 2];
        sum += task.coefs[10] * data[pos - 1];
        return data[pos] - int(arithmetic_shift_right(long(sum), task.data.shift));
    }
    if (order == 10) {
        int sum = task.coefs[0] * data[pos - 10];
        sum += task.coefs[1] * data[pos - 9];
        sum += task.coefs[2] * data[pos - 8];
        sum += task.coefs[3] * data[pos - 7];
        sum += task.coefs[4] * data[pos - 6];
        sum += task.coefs[5] * data[pos - 5];
        sum += task.coefs[6] * data[pos - 4];
        sum += task.coefs[7] * data[pos - 3];
        sum += task.coefs[8] * data[pos - 2];
        sum += task.coefs[9] * data[pos - 1];
        return data[pos] - int(arithmetic_shift_right(long(sum), task.data.shift));
    }
    int sum = 0;
    for (int i = 0; i < order; ++i) {
        sum += task.coefs[i] * data[pos - order + i];
    }
    return data[pos] - int(arithmetic_shift_right(long(sum), task.data.shift));
}

ulong fold_residual(long value)
{
    return value >= 0L
        ? ulong(value) << 1u
        : (ulong(-(value + 1L)) << 1u) + 1u;
}

int signed_value_fits_bits(int value, int bits)
{
    if (bits <= 0 || bits > 31) {
        return 0;
    }
    const int min_value = -(1 << (bits - 1));
    const int max_value = (1 << (bits - 1)) - 1;
    return value >= min_value && value <= max_value;
}

int valid_partition_order(int blocksize, int predictor_order, int partition_order)
{
    const int partition_count = 1 << partition_order;
    if ((blocksize % partition_count) != 0) {
        return 0;
    }
    return (blocksize / partition_count) > predictor_order;
}

int leading_zero_bits_u32(uint value)
{
    int count = 0;
    for (int bit = 31; bit >= 0; --bit) {
        if ((value & (1u << uint(bit))) != 0u) {
            break;
        }
        ++count;
    }
    return count;
}

int abs_bits_i32(int value)
{
    return value ^ (value >> 31);
}

int round_to_even_int(float value)
{
    return int(rint(value));
}

int generated_profile_uses_order_guess(uint analysis_profile)
{
    return analysis_profile == 1u ||
        analysis_profile == 2u ||
        analysis_profile == 3u ||
        analysis_profile == 4u ||
        analysis_profile == 5u;
}

int profile_uses_mean_rice(uint analysis_profile)
{
    return analysis_profile == 2u ||
        analysis_profile == 3u ||
        analysis_profile == 4u ||
        analysis_profile == 5u;
}

int profile_uses_mean_estimated_size(uint analysis_profile)
{
    return analysis_profile == 4u ||
        analysis_profile == 5u;
}

uint ilog2_ulong(ulong value)
{
    uint result = 0u;
    while (value > 1UL) {
        value >>= 1u;
        ++result;
    }
    return result;
}

uint mean_rice_parameter(ulong abs_sum, int sample_count)
{
    if (sample_count <= 0) {
        return 0u;
    }
    const ulong divisor = 0x40000UL / ulong(sample_count);
    const ulong scaled_mean =
        abs_sum < 2UL || divisor == 0UL ? 0UL : (((abs_sum - 1UL) * divisor) >> 18u);
    if (scaled_mean == 0UL) {
        return 0u;
    }
    return min(kMaxRiceParameter, ilog2_ulong(scaled_mean) + 1u);
}

ulong mean_rice_partition_bits(uint rice_parameter, int sample_count, ulong abs_sum)
{
    ulong bits = 4UL + (ulong(1u + rice_parameter) * ulong(sample_count));
    bits += rice_parameter == 0u
        ? (abs_sum << 1u)
        : (abs_sum >> (rice_parameter - 1u));
    const ulong correction = ulong(sample_count >> 1);
    return bits > correction ? bits - correction : 0UL;
}

kernel void find_wasted_bits(
    device const int* samples [[buffer(0)]],
    device FlacClSubframeTask* tasks [[buffer(1)]],
    constant GeneratedLpcParams& params [[buffer(2)]],
    uint lane [[thread_index_in_threadgroup]],
    uint frame [[threadgroup_position_in_grid]])
{
    threadgroup int reduce_ints[64];
    threadgroup uint reduce_uints[64];
    if (frame >= params.frameCount) {
        return;
    }
    const uint base = frame * params.tasksPerFrame;
    FlacClSubframeTask first = tasks[base];
    device const int* data = samples + first.data.samplesOffs;
    const int wbits =
        common_wasted_bits(data, first.data.blocksize, first.data.obits, reduce_ints, lane);
    const int abits =
        amplitude_bits(data, first.data.blocksize, wbits, reduce_uints, lane);

    for (uint slot = lane; slot < params.tasksPerFrame; slot += kWorkgroupSize) {
        FlacClSubframeTask task = tasks[base + slot];
        task.data.wbits = wbits;
        task.data.abits = abits;
        tasks[base + slot] = task;
    }
}

kernel void prepare_shifted_samples_for_autocorrelation(
    device const int* samples [[buffer(0)]],
    device FlacClSubframeTask* tasks [[buffer(1)]],
    constant GeneratedLpcParams& params [[buffer(2)]],
    device int* shifted_samples [[buffer(3)]],
    uint lane [[thread_index_in_threadgroup]],
    uint frame [[threadgroup_position_in_grid]])
{
    threadgroup int reduce_ints[64];
    threadgroup uint reduce_uints[64];
    if (frame >= params.frameCount) {
        return;
    }
    const uint base = frame * params.tasksPerFrame;
    FlacClSubframeTask first = tasks[base];
    device const int* data = samples + first.data.samplesOffs;
    const int wbits =
        common_wasted_bits(data, first.data.blocksize, first.data.obits, reduce_ints, lane);
    const int abits =
        amplitude_bits(data, first.data.blocksize, wbits, reduce_uints, lane);

    for (uint slot = lane; slot < params.tasksPerFrame; slot += kWorkgroupSize) {
        FlacClSubframeTask task = tasks[base + slot];
        task.data.wbits = wbits;
        task.data.abits = abits;
        tasks[base + slot] = task;
    }
    for (uint pos = lane; pos < uint(first.data.blocksize); pos += kWorkgroupSize) {
        shifted_samples[first.data.samplesOffs + pos] =
            int(shifted_sample(data, int(pos), wbits));
    }
}

kernel void compute_autocorrelation(
    device float* output [[buffer(0)]],
    device const int* samples [[buffer(1)]],
    device const float* windows [[buffer(2)]],
    device const FlacClSubframeTask* tasks [[buffer(3)]],
    constant GeneratedLpcParams& params [[buffer(4)]],
    uint lane [[thread_index_in_threadgroup]],
    uint3 group [[threadgroup_position_in_grid]])
{
    threadgroup float scratch[64];
    const uint frame = group.x;
    const uint window = group.y;
    const uint lag = group.z;
    if (frame >= params.frameCount || window >= params.windowCount || lag > kMaxOrder) {
        return;
    }

    const FlacClSubframeTask task = tasks[frame * params.tasksPerFrame];
    device const int* data = samples + task.data.samplesOffs;
    const uint window_offset = window * params.blocksize;
    const uint output_offset =
        ((frame * params.windowCount + window) * (kMaxOrder + 1u)) + lag;

    float sum = 0.0f;
    for (uint pos = lag + lane; pos < uint(task.data.blocksize); pos += kWorkgroupSize) {
        const float sample0 =
            float(shifted_sample(data, int(pos), task.data.wbits)) *
            windows[window_offset + pos];
        const float sample1 =
            float(shifted_sample(data, int(pos - lag), task.data.wbits)) *
            windows[window_offset + pos - lag];
        sum += sample0 * sample1;
    }

    const float total = reduce_sum_float(sum, scratch, lane);
    if (lane == 0u) {
        output[output_offset] = total;
    }
}

kernel void compute_autocorrelation_shifted(
    device float* output [[buffer(0)]],
    device const int* shifted_samples [[buffer(1)]],
    device const float* windows [[buffer(2)]],
    device const FlacClSubframeTask* tasks [[buffer(3)]],
    constant GeneratedLpcParams& params [[buffer(4)]],
    uint lane [[thread_index_in_threadgroup]],
    uint3 group [[threadgroup_position_in_grid]])
{
    threadgroup float scratch[64];
    const uint frame = group.x;
    const uint window = group.y;
    const uint lag = group.z;
    if (frame >= params.frameCount || window >= params.windowCount || lag > kMaxOrder) {
        return;
    }

    const FlacClSubframeTask task = tasks[frame * params.tasksPerFrame];
    device const int* data = shifted_samples + task.data.samplesOffs;
    const uint window_offset = window * params.blocksize;
    const uint output_offset =
        ((frame * params.windowCount + window) * (kMaxOrder + 1u)) + lag;

    float sum = 0.0f;
    for (uint pos = lag + lane; pos < uint(task.data.blocksize); pos += kWorkgroupSize) {
        const float sample0 = float(data[pos]) * windows[window_offset + pos];
        const float sample1 = float(data[pos - lag]) * windows[window_offset + pos - lag];
        sum += sample0 * sample1;
    }

    const float total = reduce_sum_float(sum, scratch, lane);
    if (lane == 0u) {
        output[output_offset] = total;
    }
}

kernel void compute_lpc(
    device const float* autocor [[buffer(0)]],
    device float* lpcs [[buffer(1)]],
    constant GeneratedLpcParams& params [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    const uint frame = gid.x;
    const uint window = gid.y;
    if (frame >= params.frameCount || window >= params.windowCount) {
        return;
    }

    const uint lpc_offset =
        (frame * params.windowCount + window) * (kMaxOrder + 1u) * kMaxOrder;
    const uint autocor_offset =
        (frame * params.windowCount + window) * (kMaxOrder + 1u);
    device const float* autoc = autocor + autocor_offset;

    float ldr[32];
    float gen0[32];
    float gen1[32];
    float err[32];
    const int order_limit = clamp(int(params.maxLpcOrder), 1, int(kMaxOrder));

    for (int i = 0; i < order_limit; ++i) {
        gen0[i] = autoc[i + 1];
        gen1[i] = autoc[i + 1];
        ldr[i] = 0.0f;
        err[i] = 0.0f;
    }

    float error = autoc[0];
    for (int order = 0; order < order_limit; ++order) {
        float reflection = 0.0f;
        if (error > 0.0f) {
            reflection = -gen1[0] / error;
        }
        if (!isfinite(reflection)) {
            reflection = 0.0f;
        }

        error *= 1.0f - (reflection * reflection);
        if (!isfinite(error) || error < 0.0f) {
            error = 0.0f;
        }

        for (int j = 0; j < order_limit - 1 - order; ++j) {
            const float next_gen1 = gen1[j + 1] + (reflection * gen0[j]);
            gen0[j] = (gen1[j + 1] * reflection) + gen0[j];
            gen1[j] = next_gen1;
        }

        err[order] = error;
        ldr[order] = reflection;
        for (int j = 0; j < order / 2; ++j) {
            const float tmp = ldr[j];
            ldr[j] += reflection * ldr[order - 1 - j];
            ldr[order - 1 - j] += reflection * tmp;
        }
        if ((order & 1) != 0) {
            ldr[order / 2] += ldr[order / 2] * reflection;
        }

        for (int j = 0; j <= order; ++j) {
            lpcs[lpc_offset + uint(order) * kMaxOrder + uint(j)] =
                -ldr[order - j];
        }
    }

    for (int j = 0; j < order_limit; ++j) {
        lpcs[lpc_offset + kMaxOrder * kMaxOrder + uint(j)] = err[j];
    }
}

kernel void quantize_lpc_orders(
    device FlacClSubframeTask* tasks [[buffer(0)]],
    device const float* lpcs [[buffer(1)]],
    constant GeneratedLpcParams& params [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    const uint frame = gid.x;
    const uint window = gid.y;
    if (frame >= params.frameCount || window >= params.windowCount) {
        return;
    }

    const uint lpc_offset =
        (frame * params.windowCount + window) * (kMaxOrder + 1u) * kMaxOrder;
    const uint window_task_offset = window * params.lpcTasksPerWindow;
    if (window_task_offset >= params.totalLpcTasks) {
        return;
    }
    const uint slots_this_window =
        min(params.lpcTasksPerWindow, params.totalLpcTasks - window_task_offset);

    int guessed_order = 1;
    if (generated_profile_uses_order_guess(params.analysisProfile)) {
        const int max_order = clamp(int(params.maxLpcOrder), 1, int(kMaxOrder));
        const FlacClSubframeTask first_task = tasks[frame * params.tasksPerFrame];
        const float error_scale = 0.5f / float(first_task.data.blocksize);
        float best_bits = 3.402823466e+38f;
        const int overhead_bits_per_order =
            max(1, first_task.data.obits - first_task.data.wbits) +
            clamp(int(params.coefficientPrecision), 1, 15);
        for (int order = 1; order <= max_order; ++order) {
            const float error = lpcs[lpc_offset + kMaxOrder * kMaxOrder + uint(order - 1)];
            float bits_per_residual = 0.0f;
            if (error > 0.0f) {
                bits_per_residual = max(0.0f, 0.5f * log2(error_scale * error));
            } else if (error < 0.0f) {
                bits_per_residual = 8.507058665e+37f;
            }
            const float residual_samples =
                max(0.0f, float(first_task.data.blocksize - order));
            const float bits =
                (bits_per_residual * residual_samples) +
                float(order * overhead_bits_per_order);
            if (bits < best_bits) {
                best_bits = bits;
                guessed_order = order;
            }
        }
    }

    for (uint slot = 0u; slot < slots_this_window; ++slot) {
        const uint task_no = frame * params.tasksPerFrame + window_task_offset + slot;
        FlacClSubframeTask task = tasks[task_no];
        int order = task.data.residualOrder;
        if (generated_profile_uses_order_guess(params.analysisProfile)) {
            if (params.lpcTasksPerWindow == 1u) {
                order = guessed_order;
                task.data.residualOrder = guessed_order;
            } else if (order != guessed_order) {
                task.data.size = kIntMax;
                tasks[task_no] = task;
                continue;
            }
        }

        int max_coefficient = 0;
        for (int i = 0; i < order; ++i) {
            const float lpc = lpcs[lpc_offset + uint(order - 1) * kMaxOrder + uint(i)];
            const int coefficient = round_to_even_int(lpc * float(1 << 15));
            max_coefficient |= abs_bits_i32(coefficient);
        }

        const int desired_bits = clamp(int(params.coefficientPrecision), 1, 15);
        int shift = 0;
        if (max_coefficient != 0) {
            shift = clamp(
                leading_zero_bits_u32(uint(max_coefficient)) - 18 + desired_bits,
                0,
                15);
        }
        const int limit = (1 << (desired_bits - 1)) - 1;

        int actual_max_coefficient = 0;
        for (uint i = 0u; i < kMaxOrder; ++i) {
            task.coefs[i] = 0;
        }
        for (int i = 0; i < order; ++i) {
            const float lpc = lpcs[lpc_offset + uint(order - 1) * kMaxOrder + uint(i)];
            const int coefficient = clamp(
                round_to_even_int(lpc * float(1 << shift)), -limit, limit);
            actual_max_coefficient |= abs_bits_i32(coefficient);
            task.coefs[i] = coefficient;
        }

        task.data.residualOrder = order;
        task.data.shift = shift;
        task.data.cbits = actual_max_coefficient == 0
            ? 1
            : 1 + 32 - leading_zero_bits_u32(uint(actual_max_coefficient));
        tasks[task_no] = task;
    }
}

ulong abs_long(long value)
{
    return value >= 0L ? ulong(value) : ulong(-(value + 1L)) + 1UL;
}

kernel void prune_fixed_order_guess(
    device const int* samples [[buffer(0)]],
    device const int* selected_tasks [[buffer(1)]],
    device FlacClSubframeTask* tasks [[buffer(2)]],
    constant GeneratedLpcParams& params [[buffer(3)]],
    device const int* shifted_samples [[buffer(4)]],
    uint lane [[thread_index_in_threadgroup]],
    uint frame [[threadgroup_position_in_grid]])
{
    threadgroup ulong reduce_scratch[5 * 64];
    if (frame >= params.frameCount) {
        return;
    }
    const uint base = frame * params.tasksPerFrame;
    int fixed_task_no[5];
    for (uint order = 0u; order <= 4u; ++order) {
        fixed_task_no[order] = -1;
    }

    for (uint i = 0u; i < params.tasksPerFrame; ++i) {
        const int task_no = selected_tasks[base + i];
        FlacClSubframeTask task = tasks[task_no];
        const int order = task.data.residualOrder;
        if (task.data.type == kSubframeFixed && order >= 0 && order <= 4) {
            fixed_task_no[order] = task_no;
        }
    }

    FlacClSubframeTask first_task = tasks[selected_tasks[base]];
    const int bs = first_task.data.blocksize;
    const int wbits = first_task.data.wbits;
    device const int* data = samples + first_task.data.samplesOffs;
    device const int* shifted_data =
        shifted_samples + first_task.data.samplesOffs;

    ulong local_sums[5];
    for (uint order = 0u; order <= 4u; ++order) {
        local_sums[order] = 0UL;
    }

    if (params.shiftedSamplesReady != 0u) {
        for (int pos = int(lane); pos < bs; pos += int(kWorkgroupSize)) {
            const long sample0 = long(shifted_data[pos]);
            const long sample1 = pos >= 1 ? long(shifted_data[pos - 1]) : 0L;
            const long sample2 = pos >= 2 ? long(shifted_data[pos - 2]) : 0L;
            const long sample3 = pos >= 3 ? long(shifted_data[pos - 3]) : 0L;
            const long sample4 = pos >= 4 ? long(shifted_data[pos - 4]) : 0L;
            if (fixed_task_no[0] >= 0) {
                local_sums[0] += abs_long(sample0);
            }
            if (fixed_task_no[1] >= 0 && pos >= 1) {
                local_sums[1] += abs_long(sample0 - sample1);
            }
            if (fixed_task_no[2] >= 0 && pos >= 2) {
                local_sums[2] += abs_long(sample0 - (2L * sample1) + sample2);
            }
            if (fixed_task_no[3] >= 0 && pos >= 3) {
                local_sums[3] += abs_long(
                    sample0 - (3L * sample1) + (3L * sample2) - sample3);
            }
            if (fixed_task_no[4] >= 0 && pos >= 4) {
                local_sums[4] += abs_long(
                    sample0 - (4L * sample1) + (6L * sample2) -
                    (4L * sample3) + sample4);
            }
        }
    } else {
        for (int pos = int(lane); pos < bs; pos += int(kWorkgroupSize)) {
            const long sample0 = shifted_sample(data, pos, wbits);
            const long sample1 = pos >= 1
                ? shifted_sample(data, pos - 1, wbits)
                : 0L;
            const long sample2 = pos >= 2
                ? shifted_sample(data, pos - 2, wbits)
                : 0L;
            const long sample3 = pos >= 3
                ? shifted_sample(data, pos - 3, wbits)
                : 0L;
            const long sample4 = pos >= 4
                ? shifted_sample(data, pos - 4, wbits)
                : 0L;
            if (fixed_task_no[0] >= 0) {
                local_sums[0] += abs_long(sample0);
            }
            if (fixed_task_no[1] >= 0 && pos >= 1) {
                local_sums[1] += abs_long(sample0 - sample1);
            }
            if (fixed_task_no[2] >= 0 && pos >= 2) {
                local_sums[2] += abs_long(sample0 - (2L * sample1) + sample2);
            }
            if (fixed_task_no[3] >= 0 && pos >= 3) {
                local_sums[3] += abs_long(
                    sample0 - (3L * sample1) + (3L * sample2) - sample3);
            }
            if (fixed_task_no[4] >= 0 && pos >= 4) {
                local_sums[4] += abs_long(
                    sample0 - (4L * sample1) + (6L * sample2) -
                    (4L * sample3) + sample4);
            }
        }
    }

    ulong sums[5];
    for (uint order = 0u; order <= 4u; ++order) {
        sums[order] = reduce_sum_u64(
            local_sums[order], reduce_scratch + (order * kWorkgroupSize), lane);
    }

    if (lane != 0u) {
        return;
    }

    int best_order = -1;
    ulong best_sum = 0xffffffffffffffffUL;
    for (uint order = 0u; order <= 4u; ++order) {
        if (fixed_task_no[order] >= 0 && sums[order] < best_sum) {
            best_order = int(order);
            best_sum = sums[order];
        }
    }
    if (best_order < 0) {
        return;
    }

    for (uint order = 0u; order <= 4u; ++order) {
        if (fixed_task_no[order] >= 0 && int(order) != best_order) {
            FlacClSubframeTask task = tasks[fixed_task_no[order]];
            task.data.size = kIntMax;
            tasks[fixed_task_no[order]] = task;
        }
    }
}

RiceChoice best_rice_for_partition(device const int* data, int partition_start,
    int residual_count, thread const FlacClSubframeTask& task, threadgroup ulong* scratch,
    uint lane)
{
    RiceChoice best;
    best.bits = 0xffffffffffffffffUL;
    best.parameter = 0u;
    for (uint parameter = 0u; parameter <= kMaxRiceParameter; ++parameter) {
        ulong local_bits = 0UL;
        for (int i = int(lane); i < residual_count; i += int(kWorkgroupSize)) {
            const ulong folded = fold_residual(residual(data, partition_start + i, task));
            local_bits += (folded >> parameter) + 1UL + ulong(parameter);
        }
        const ulong bits = reduce_sum_u64(local_bits, scratch, lane);
        if (bits < best.bits) {
            best.bits = bits;
            best.parameter = parameter;
        }
    }
    return best;
}

MeanRiceChoice mean_rice_for_partition(device const int* data, int partition_start,
    int residual_count, thread const FlacClSubframeTask& task, threadgroup ulong* scratch,
    uint lane, int include_exact_bits)
{
    ulong local_abs_sum = 0UL;
    for (int i = int(lane); i < residual_count; i += int(kWorkgroupSize)) {
        local_abs_sum += abs_long(residual(data, partition_start + i, task));
    }

    const ulong abs_sum = reduce_sum_u64(local_abs_sum, scratch, lane);
    const uint parameter = mean_rice_parameter(abs_sum, residual_count);

    ulong local_exact_bits = 0UL;
    if (include_exact_bits) {
        for (int i = int(lane); i < residual_count; i += int(kWorkgroupSize)) {
            const ulong folded = fold_residual(residual(data, partition_start + i, task));
            local_exact_bits += (folded >> parameter) + 1UL + ulong(parameter);
        }
    }

    MeanRiceChoice choice;
    choice.exactBits = include_exact_bits
        ? reduce_sum_u64(local_exact_bits, scratch, lane)
        : 0UL;
    choice.estimatedBits =
        mean_rice_partition_bits(parameter, residual_count, abs_sum);
    choice.parameter = parameter;
    return choice;
}

kernel void analyze_exact(
    device const int* samples [[buffer(0)]],
    device const int* selected_tasks [[buffer(1)]],
    device FlacClSubframeTask* tasks [[buffer(2)]],
    device uint* task_rice_parameters [[buffer(3)]],
    constant ExactParams& params [[buffer(4)]],
    uint lane [[thread_index_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]])
{
    threadgroup int reduce_ints[64];
    threadgroup uint reduce_uints[64];
    threadgroup ulong reduce_ulongs[64];
    threadgroup ulong exact_rice_leaf_sums[64 * 15];
    threadgroup uint candidate_rice_parameters[256];
    threadgroup uint best_rice_parameters[256];

    if (group >= params.selectedTaskCount) {
        return;
    }

    const int selected_task = selected_tasks[group];
    FlacClSubframeTask task = tasks[selected_task];
    const int rice_output_base = selected_task * int(kMaxRicePartitionCount);
    for (int i = int(lane); i < int(kMaxRicePartitionCount); i += int(kWorkgroupSize)) {
        task_rice_parameters[rice_output_base + i] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (task.data.size == kIntMax) {
        if (lane == 0u) {
            tasks[selected_task] = task;
        }
        return;
    }

    device const int* data = samples + task.data.samplesOffs;
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
    if (params.preparedFrameFacts == 0u) {
        task.data.wbits = common_wasted_bits(data, bs, task.data.obits, reduce_ints, lane);
        task.data.abits = amplitude_bits(data, bs, task.data.wbits, reduce_uints, lane);
    } else if (task.data.wbits < 0 || task.data.wbits >= task.data.obits ||
        task.data.abits <= 0 || task.data.abits > task.data.obits) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }
    task.data.porder = 0;
    const int wbits = task.data.wbits;
    const int obits = task.data.obits - wbits;

    if (task.data.type == kSubframeConstant) {
        const int equal = all_samples_equal(data, bs, reduce_uints, lane);
        if (lane == 0u) {
            task.data.size = equal
                ? 8 + obits + (wbits == 0 ? 0 : wbits)
                : kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if ((task.data.type != kSubframeFixed && task.data.type != kSubframeLpc) ||
        ro < 0 ||
        ro >= bs ||
        (task.data.type == kSubframeFixed && ro > 4) ||
        (task.data.type == kSubframeLpc &&
            (ro == 0 || ro > int(kMaxOrder) || task.data.shift < 0 ||
                task.data.shift > 15 || task.data.cbits <= 0 ||
                task.data.cbits > 15))) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if (task.data.type == kSubframeLpc) {
        int valid = 1;
        for (int i = 0; i < ro; ++i) {
            if (!signed_value_fits_bits(task.coefs[i], task.data.cbits)) {
                valid = 0;
            }
        }
        if (!valid) {
            if (lane == 0u) {
                task.data.size = kIntMax;
                tasks[selected_task] = task;
            }
            return;
        }
    }

    ulong best_bits = 0xffffffffffffffffUL;
    ulong best_estimated_bits = 0xffffffffffffffffUL;
    int best_partition_order = 0;
    const int max_partition_order = min(int(params.maxRicePartitionOrder), 8);
    const int use_mean_rice = profile_uses_mean_rice(params.analysisProfile);
    const int use_mean_estimated_size =
        profile_uses_mean_estimated_size(params.analysisProfile);
    const int use_leaf_exact_rice =
        !use_mean_rice &&
        max_partition_order <= int(kExactLeafMaxRicePartitionOrder) &&
        valid_partition_order(bs, ro, max_partition_order);
    const int use_leaf_mean_estimated_rice =
        use_mean_rice &&
        use_mean_estimated_size &&
        max_partition_order <= int(kExactLeafMaxRicePartitionOrder) &&
        valid_partition_order(bs, ro, max_partition_order);
    const ulong base_bits = task.data.type == kSubframeLpc
        ? 8UL + (wbits == 0 ? 0UL : ulong(wbits)) +
            (ulong(ro) * ulong(obits)) + 4UL + 5UL +
            (ulong(ro) * ulong(task.data.cbits)) + 2UL + 4UL
        : 8UL + (ulong(ro) * ulong(obits)) + 2UL + 4UL +
            (wbits == 0 ? 0UL : ulong(wbits));

    if (use_leaf_exact_rice) {
        const int leaf_partition_order = max_partition_order;
        const int leaf_count = 1 << leaf_partition_order;
        const int leaf_samples = bs >> leaf_partition_order;
        for (int leaf = int(lane); leaf < leaf_count; leaf += int(kWorkgroupSize)) {
            ulong sums[15];
            for (uint parameter = 0u; parameter < kExactLeafRiceParameterCount; ++parameter) {
                sums[parameter] = 0UL;
            }

            const int start = leaf == 0 ? ro : leaf * leaf_samples;
            const int end = (leaf + 1) * leaf_samples;
            for (int pos = start; pos < end; ++pos) {
                const ulong folded = fold_residual(residual(data, pos, task));
                for (uint parameter = 0u; parameter < kExactLeafRiceParameterCount;
                     ++parameter) {
                    sums[parameter] += folded >> parameter;
                }
            }

            const int output_base = leaf * int(kExactLeafRiceParameterCount);
            for (uint parameter = 0u; parameter < kExactLeafRiceParameterCount; ++parameter) {
                exact_rice_leaf_sums[output_base + int(parameter)] = sums[parameter];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (lane == 0u) {
            for (int partition_order = 0; partition_order <= leaf_partition_order;
                 ++partition_order) {
                if (!valid_partition_order(bs, ro, partition_order)) {
                    continue;
                }

                const int partition_count = 1 << partition_order;
                const int partition_samples = bs >> partition_order;
                const int leaf_group_size = 1 << (leaf_partition_order - partition_order);
                ulong bits = base_bits;
                for (int partition = 0; partition < partition_count; ++partition) {
                    const int residual_count = partition == 0
                        ? partition_samples - ro
                        : partition_samples;
                    const int leaf_start = partition * leaf_group_size;
                    ulong best_parameter_bits = 0xffffffffffffffffUL;
                    uint best_parameter = 0u;
                    for (uint parameter = 0u; parameter <= kMaxRiceParameter; ++parameter) {
                        ulong sum = 0UL;
                        for (int leaf = 0; leaf < leaf_group_size; ++leaf) {
                            const int index =
                                ((leaf_start + leaf) * int(kExactLeafRiceParameterCount)) +
                                int(parameter);
                            sum += exact_rice_leaf_sums[index];
                        }
                        const ulong parameter_bits =
                            sum + (ulong(residual_count) * ulong(1u + parameter));
                        if (parameter_bits < best_parameter_bits) {
                            best_parameter_bits = parameter_bits;
                            best_parameter = parameter;
                        }
                    }
                    bits += 4UL + best_parameter_bits;
                    candidate_rice_parameters[partition] = best_parameter;
                }

                if (bits < best_bits) {
                    best_bits = bits;
                    best_partition_order = partition_order;
                    for (int partition = 0; partition < partition_count; ++partition) {
                        task_rice_parameters[rice_output_base + partition] =
                            candidate_rice_parameters[partition];
                    }
                }
            }

            task.data.porder = best_partition_order;
            task.data.size = best_bits > ulong(kIntMax) ? kIntMax : int(best_bits);
            tasks[selected_task] = task;
        }
        return;
    }

    if (use_leaf_mean_estimated_rice) {
        const int leaf_partition_order = max_partition_order;
        const int leaf_count = 1 << leaf_partition_order;
        const int leaf_samples = bs >> leaf_partition_order;
        for (int leaf = int(lane); leaf < leaf_count; leaf += int(kWorkgroupSize)) {
            ulong abs_sum = 0UL;
            const int start = leaf == 0 ? ro : leaf * leaf_samples;
            const int end = (leaf + 1) * leaf_samples;
            for (int pos = start; pos < end; ++pos) {
                abs_sum += abs_long(residual(data, pos, task));
            }
            exact_rice_leaf_sums[leaf] = abs_sum;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (lane == 0u) {
            for (int partition_order = leaf_partition_order; partition_order >= 0;
                 --partition_order) {
                if (!valid_partition_order(bs, ro, partition_order)) {
                    continue;
                }

                const int partition_count = 1 << partition_order;
                const int partition_samples = bs >> partition_order;
                const int leaf_group_size = 1 << (leaf_partition_order - partition_order);
                ulong estimated_bits = 2UL + 4UL;
                for (int partition = 0; partition < partition_count; ++partition) {
                    const int residual_count = partition == 0
                        ? partition_samples - ro
                        : partition_samples;
                    const int leaf_start = partition * leaf_group_size;
                    ulong abs_sum = 0UL;
                    for (int leaf = 0; leaf < leaf_group_size; ++leaf) {
                        abs_sum += exact_rice_leaf_sums[leaf_start + leaf];
                    }

                    const uint parameter = mean_rice_parameter(abs_sum, residual_count);
                    estimated_bits +=
                        mean_rice_partition_bits(parameter, residual_count, abs_sum);
                    candidate_rice_parameters[partition] = parameter;
                }

                if (estimated_bits < best_estimated_bits) {
                    best_estimated_bits = estimated_bits;
                    best_partition_order = partition_order;
                    const ulong rice_header_bits = 2UL + 4UL;
                    const ulong estimate_base_bits = base_bits - rice_header_bits;
                    best_bits = best_estimated_bits >=
                            0xffffffffffffffffUL - estimate_base_bits
                        ? 0xffffffffffffffffUL
                        : estimate_base_bits + best_estimated_bits;
                    for (int partition = 0; partition < partition_count; ++partition) {
                        task_rice_parameters[rice_output_base + partition] =
                            candidate_rice_parameters[partition];
                    }
                }
            }

            task.data.porder = best_partition_order;
            task.data.size = best_bits > ulong(kIntMax) ? kIntMax : int(best_bits);
            tasks[selected_task] = task;
        }
        return;
    }

    const int partition_start_order = use_mean_rice ? max_partition_order : 0;
    const int partition_end_order = use_mean_rice ? 0 : max_partition_order;
    const int partition_step = use_mean_rice ? -1 : 1;
    for (int partition_order = partition_start_order;
         use_mean_rice ? partition_order >= partition_end_order
                       : partition_order <= partition_end_order;
         partition_order += partition_step) {
        if (!valid_partition_order(bs, ro, partition_order)) {
            continue;
        }
        const int partition_count = 1 << partition_order;
        const int partition_samples = bs >> partition_order;
        ulong bits = base_bits;
        ulong estimated_bits = 2UL + 4UL;
        for (int partition = 0; partition < partition_count; ++partition) {
            const int residual_count = partition == 0
                ? partition_samples - ro
                : partition_samples;
            const int partition_start = partition == 0
                ? ro
                : partition * partition_samples;
            if (use_mean_rice) {
                const MeanRiceChoice choice =
                    mean_rice_for_partition(data, partition_start, residual_count,
                        task, reduce_ulongs, lane, 0);
                estimated_bits += choice.estimatedBits;
                if (lane == 0u) {
                    candidate_rice_parameters[partition] = choice.parameter;
                }
            } else {
                const RiceChoice choice =
                    best_rice_for_partition(data, partition_start, residual_count,
                        task, reduce_ulongs, lane);
                bits += 4UL + choice.bits;
                if (lane == 0u) {
                    candidate_rice_parameters[partition] = choice.parameter;
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if ((use_mean_rice && estimated_bits < best_estimated_bits) ||
            (!use_mean_rice && bits < best_bits)) {
            best_bits = bits;
            best_estimated_bits = estimated_bits;
            best_partition_order = partition_order;
            for (int partition = int(lane); partition < partition_count;
                 partition += int(kWorkgroupSize)) {
                best_rice_parameters[partition] = candidate_rice_parameters[partition];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (use_mean_rice) {
        if (use_mean_estimated_size) {
            const ulong rice_header_bits = 2UL + 4UL;
            const ulong estimate_base_bits = base_bits - rice_header_bits;
            best_bits = best_estimated_bits >= 0xffffffffffffffffUL - estimate_base_bits
                ? 0xffffffffffffffffUL
                : estimate_base_bits + best_estimated_bits;
        } else {
            best_bits = base_bits;
            const int partition_count = 1 << best_partition_order;
            const int partition_samples = bs >> best_partition_order;
            for (int partition = 0; partition < partition_count; ++partition) {
                const int residual_count = partition == 0
                    ? partition_samples - ro
                    : partition_samples;
                const int partition_start = partition == 0
                    ? ro
                    : partition * partition_samples;
                const MeanRiceChoice choice =
                    mean_rice_for_partition(data, partition_start, residual_count,
                        task, reduce_ulongs, lane, 1);
                best_bits += 4UL + choice.exactBits;
            }
        }
    }

    const int best_partition_count = 1 << best_partition_order;
    for (int partition = int(lane); partition < best_partition_count;
         partition += int(kWorkgroupSize)) {
        task_rice_parameters[rice_output_base + partition] = best_rice_parameters[partition];
    }

    if (lane == 0u) {
        task.data.porder = best_partition_order;
        task.data.size = best_bits > ulong(kIntMax) ? kIntMax : int(best_bits);
        tasks[selected_task] = task;
    }
}

kernel void analyze_exact_shifted(
    device const int* samples [[buffer(0)]],
    device const int* selected_tasks [[buffer(1)]],
    device FlacClSubframeTask* tasks [[buffer(2)]],
    device uint* task_rice_parameters [[buffer(3)]],
    constant ExactParams& params [[buffer(4)]],
    device const int* shifted_samples [[buffer(5)]],
    uint lane [[thread_index_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]])
{
    threadgroup uint reduce_uints[64];
    threadgroup ulong exact_rice_leaf_sums[64];
    threadgroup uint candidate_rice_parameters[256];

    if (group >= params.selectedTaskCount) {
        return;
    }

    const int selected_task = selected_tasks[group];
    FlacClSubframeTask task = tasks[selected_task];
    const int rice_output_base = selected_task * int(kMaxRicePartitionCount);
    for (int i = int(lane); i < int(kMaxRicePartitionCount); i += int(kWorkgroupSize)) {
        task_rice_parameters[rice_output_base + i] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (task.data.size == kIntMax) {
        if (lane == 0u) {
            tasks[selected_task] = task;
        }
        return;
    }

    device const int* original_data = samples + task.data.samplesOffs;
    device const int* data = shifted_samples + task.data.samplesOffs;
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
    if (params.preparedFrameFacts == 0u ||
        task.data.wbits < 0 || task.data.wbits >= task.data.obits ||
        task.data.abits <= 0 || task.data.abits > task.data.obits ||
        !profile_uses_mean_estimated_size(params.analysisProfile)) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }
    task.data.porder = 0;
    const int wbits = task.data.wbits;
    const int obits = task.data.obits - wbits;

    if (task.data.type == kSubframeConstant) {
        const int equal = all_samples_equal(original_data, bs, reduce_uints, lane);
        if (lane == 0u) {
            task.data.size = equal
                ? 8 + obits + (wbits == 0 ? 0 : wbits)
                : kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if ((task.data.type != kSubframeFixed && task.data.type != kSubframeLpc) ||
        ro < 0 ||
        ro >= bs ||
        (task.data.type == kSubframeFixed && ro > 4) ||
        (task.data.type == kSubframeLpc &&
            (ro == 0 || ro > int(kMaxOrder) || task.data.shift < 0 ||
                task.data.shift > 15 || task.data.cbits <= 0 ||
                task.data.cbits > 15))) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    if (task.data.type == kSubframeLpc) {
        int valid = 1;
        for (int i = 0; i < ro; ++i) {
            if (!signed_value_fits_bits(task.coefs[i], task.data.cbits)) {
                valid = 0;
            }
        }
        if (!valid) {
            if (lane == 0u) {
                task.data.size = kIntMax;
                tasks[selected_task] = task;
            }
            return;
        }
    }

    const int max_partition_order = min(int(params.maxRicePartitionOrder), 8);
    if (max_partition_order > int(kExactLeafMaxRicePartitionOrder) ||
        !valid_partition_order(bs, ro, max_partition_order)) {
        if (lane == 0u) {
            task.data.size = kIntMax;
            tasks[selected_task] = task;
        }
        return;
    }

    FlacClSubframeTask residual_task = task;
    residual_task.data.wbits = 0;
    const ulong base_bits = task.data.type == kSubframeLpc
        ? 8UL + (wbits == 0 ? 0UL : ulong(wbits)) +
            (ulong(ro) * ulong(obits)) + 4UL + 5UL +
            (ulong(ro) * ulong(task.data.cbits)) + 2UL + 4UL
        : 8UL + (ulong(ro) * ulong(obits)) + 2UL + 4UL +
            (wbits == 0 ? 0UL : ulong(wbits));

    const int leaf_partition_order = max_partition_order;
    const int leaf_count = 1 << leaf_partition_order;
    const int leaf_samples = bs >> leaf_partition_order;

    int use_i32_lpc = task.data.type == kSubframeLpc && task.data.abits < 31;
    if (use_i32_lpc) {
        ulong coefficient_abs_sum = 0UL;
        for (int i = 0; i < ro; ++i) {
            coefficient_abs_sum += abs_long(long(task.coefs[i]));
        }
        const ulong max_sample = 1UL << uint(task.data.abits);
        use_i32_lpc =
            coefficient_abs_sum <=
                (ulong(kIntMax) - max_sample) / max_sample;
    }

    if (use_i32_lpc) {
        for (int leaf = int(lane); leaf < leaf_count; leaf += int(kWorkgroupSize)) {
            ulong abs_sum = 0UL;
            const int start = leaf == 0 ? ro : leaf * leaf_samples;
            const int end = (leaf + 1) * leaf_samples;
            for (int pos = start; pos < end; ++pos) {
                abs_sum += abs_long(long(lpc_residual_shifted_i32(data, pos, task)));
            }
            exact_rice_leaf_sums[leaf] = abs_sum;
        }
    } else {
        for (int leaf = int(lane); leaf < leaf_count; leaf += int(kWorkgroupSize)) {
            ulong abs_sum = 0UL;
            const int start = leaf == 0 ? ro : leaf * leaf_samples;
            const int end = (leaf + 1) * leaf_samples;
            for (int pos = start; pos < end; ++pos) {
                abs_sum += abs_long(residual(data, pos, residual_task));
            }
            exact_rice_leaf_sums[leaf] = abs_sum;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (lane == 0u) {
        ulong best_bits = 0xffffffffffffffffUL;
        ulong best_estimated_bits = 0xffffffffffffffffUL;
        int best_partition_order = 0;
        for (int partition_order = leaf_partition_order; partition_order >= 0;
             --partition_order) {
            if (!valid_partition_order(bs, ro, partition_order)) {
                continue;
            }

            const int partition_count = 1 << partition_order;
            const int partition_samples = bs >> partition_order;
            const int leaf_group_size = 1 << (leaf_partition_order - partition_order);
            ulong estimated_bits = 2UL + 4UL;
            for (int partition = 0; partition < partition_count; ++partition) {
                const int residual_count = partition == 0
                    ? partition_samples - ro
                    : partition_samples;
                const int leaf_start = partition * leaf_group_size;
                ulong abs_sum = 0UL;
                for (int leaf = 0; leaf < leaf_group_size; ++leaf) {
                    abs_sum += exact_rice_leaf_sums[leaf_start + leaf];
                }

                const uint parameter = mean_rice_parameter(abs_sum, residual_count);
                estimated_bits +=
                    mean_rice_partition_bits(parameter, residual_count, abs_sum);
                candidate_rice_parameters[partition] = parameter;
            }

            if (estimated_bits < best_estimated_bits) {
                best_estimated_bits = estimated_bits;
                best_partition_order = partition_order;
                const ulong rice_header_bits = 2UL + 4UL;
                const ulong estimate_base_bits = base_bits - rice_header_bits;
                best_bits = best_estimated_bits >=
                        0xffffffffffffffffUL - estimate_base_bits
                    ? 0xffffffffffffffffUL
                    : estimate_base_bits + best_estimated_bits;
                for (int partition = 0; partition < partition_count; ++partition) {
                    task_rice_parameters[rice_output_base + partition] =
                        candidate_rice_parameters[partition];
                }
            }
        }

        task.data.porder = best_partition_order;
        task.data.size = best_bits > ulong(kIntMax) ? kIntMax : int(best_bits);
        tasks[selected_task] = task;
    }
}

kernel void choose_best(
    device const int* selected_tasks [[buffer(0)]],
    device const FlacClSubframeTask* tasks [[buffer(1)]],
    device const uint* task_rice_parameters [[buffer(2)]],
    device FlacClSubframeTask* best_tasks [[buffer(3)]],
    device uint* best_rice_parameters [[buffer(4)]],
    constant ChooseParams& params [[buffer(5)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= params.frameCount) {
        return;
    }
    const uint selected_base = gid * params.tasksPerFrame;
    int best_task = selected_tasks[selected_base];
    int best_size = tasks[best_task].data.size;
    for (uint i = 1u; i < params.tasksPerFrame; ++i) {
        const int task = selected_tasks[selected_base + i];
        const int size = tasks[task].data.size;
        if (size < best_size) {
            best_task = task;
            best_size = size;
        }
    }
    best_tasks[gid] = tasks[best_task];
    const uint input_base = uint(best_task) * kMaxRicePartitionCount;
    const uint output_base = gid * kMaxRicePartitionCount;
    for (uint i = 0u; i < kMaxRicePartitionCount; ++i) {
        best_rice_parameters[output_base + i] = task_rice_parameters[input_base + i];
    }
}
)MSL";
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

id<MTLDevice> copy_selected_device(std::optional<std::size_t> requested_index)
{
    const auto selected = select_metal_device(requested_index);
    NSArray<id<MTLDevice>>* devices = copy_all_devices();
    if (selected.index >= [devices count]) {
        throw std::runtime_error("selected Metal device disappeared during setup");
    }
    id<MTLDevice> device = [devices objectAtIndex:selected.index];
    if (device == nil) {
        throw std::runtime_error("selected Metal device is nil");
    }
    return device;
}

id<MTLBuffer> ensure_buffer(
    id<MTLDevice> device,
    id<MTLBuffer> buffer,
    std::size_t& capacity_bytes,
    std::size_t required_bytes,
    const char* name)
{
    if (required_bytes == 0) {
        throw std::runtime_error(std::string("Metal ") + name + " buffer is empty");
    }
    if (required_bytes <= capacity_bytes && buffer != nil) {
        return buffer;
    }
    buffer = [device newBufferWithLength:required_bytes options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw std::runtime_error(std::string("failed to allocate Metal ") + name + " buffer");
    }
    capacity_bytes = required_bytes;
    return buffer;
}

void copy_to_buffer(id<MTLBuffer> buffer, const void* data, std::size_t bytes)
{
    std::memcpy([buffer contents], data, bytes);
}

void copy_from_buffer(void* data, id<MTLBuffer> buffer, std::size_t bytes)
{
    std::memcpy(data, [buffer contents], bytes);
}

}  // namespace

class MetalMonoAnalysisSession::Impl final {
public:
    explicit Impl(std::optional<std::size_t> requested_device_index)
    {
        @autoreleasepool {
            device_ = copy_selected_device(requested_device_index);
            device_name_ = ns_string_to_string([device_ name]);
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                throw std::runtime_error("failed to create Metal command queue");
            }

            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:metal_analysis_source()];
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:source options:nil error:&error];
            if (library == nil) {
                throw std::runtime_error("failed to compile Metal analysis source: " +
                    ns_error_text(error));
            }

            id<MTLFunction> find_wasted_function = [library newFunctionWithName:@"find_wasted_bits"];
            id<MTLFunction> prepare_shifted_function =
                [library newFunctionWithName:@"prepare_shifted_samples_for_autocorrelation"];
            id<MTLFunction> autocor_function =
                [library newFunctionWithName:@"compute_autocorrelation"];
            id<MTLFunction> autocor_shifted_function =
                [library newFunctionWithName:@"compute_autocorrelation_shifted"];
            id<MTLFunction> lpc_function = [library newFunctionWithName:@"compute_lpc"];
            id<MTLFunction> quantize_function =
                [library newFunctionWithName:@"quantize_lpc_orders"];
            id<MTLFunction> fixed_guess_function =
                [library newFunctionWithName:@"prune_fixed_order_guess"];
            id<MTLFunction> exact_function = [library newFunctionWithName:@"analyze_exact"];
            id<MTLFunction> exact_shifted_function =
                [library newFunctionWithName:@"analyze_exact_shifted"];
            id<MTLFunction> choose_function = [library newFunctionWithName:@"choose_best"];
            if (find_wasted_function == nil || prepare_shifted_function == nil ||
                autocor_function == nil || autocor_shifted_function == nil ||
                lpc_function == nil || quantize_function == nil ||
                fixed_guess_function == nil || exact_function == nil ||
                exact_shifted_function == nil ||
                choose_function == nil) {
                throw std::runtime_error("failed to resolve Metal analysis kernel functions");
            }

            find_wasted_pipeline_ =
                [device_ newComputePipelineStateWithFunction:find_wasted_function error:&error];
            if (find_wasted_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal wasted-bits pipeline: " +
                    ns_error_text(error));
            }
            prepare_shifted_pipeline_ =
                [device_ newComputePipelineStateWithFunction:prepare_shifted_function
                                                       error:&error];
            if (prepare_shifted_pipeline_ == nil) {
                throw std::runtime_error(
                    "failed to create Metal shifted-sample prepare pipeline: " +
                    ns_error_text(error));
            }
            autocor_pipeline_ =
                [device_ newComputePipelineStateWithFunction:autocor_function error:&error];
            if (autocor_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal autocorrelation pipeline: " +
                    ns_error_text(error));
            }
            autocor_shifted_pipeline_ =
                [device_ newComputePipelineStateWithFunction:autocor_shifted_function
                                                       error:&error];
            if (autocor_shifted_pipeline_ == nil) {
                throw std::runtime_error(
                    "failed to create Metal shifted autocorrelation pipeline: " +
                    ns_error_text(error));
            }
            lpc_pipeline_ =
                [device_ newComputePipelineStateWithFunction:lpc_function error:&error];
            if (lpc_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal LPC pipeline: " +
                    ns_error_text(error));
            }
            quantize_pipeline_ =
                [device_ newComputePipelineStateWithFunction:quantize_function error:&error];
            if (quantize_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal quantize pipeline: " +
                    ns_error_text(error));
            }
            fixed_guess_pipeline_ =
                [device_ newComputePipelineStateWithFunction:fixed_guess_function error:&error];
            if (fixed_guess_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal fixed-order pipeline: " +
                    ns_error_text(error));
            }
            exact_pipeline_ =
                [device_ newComputePipelineStateWithFunction:exact_function error:&error];
            if (exact_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal exact-analysis pipeline: " +
                    ns_error_text(error));
            }
            exact_shifted_pipeline_ =
                [device_ newComputePipelineStateWithFunction:exact_shifted_function
                                                       error:&error];
            if (exact_shifted_pipeline_ == nil) {
                throw std::runtime_error(
                    "failed to create Metal shifted exact-analysis pipeline: " +
                    ns_error_text(error));
            }
            choose_pipeline_ =
                [device_ newComputePipelineStateWithFunction:choose_function error:&error];
            if (choose_pipeline_ == nil) {
                throw std::runtime_error("failed to create Metal choose-best pipeline: " +
                    ns_error_text(error));
            }
        }
    }

    OpenClMonoFixedConstantAnalysisResult run_analysis(
        const std::vector<std::int32_t>& samples,
        OpenClMonoAnalysisTaskPlan plan,
        unsigned lpc_coefficient_precision,
        unsigned max_rice_partition_order,
        bool populate_lpc,
        bool read_analyzed_tasks,
        MetalGpuTimingStats* timings)
    {
        @autoreleasepool {
            if (max_rice_partition_order > opencl_detail::kFlacClMaxRicePartitionOrder) {
                throw std::runtime_error("Metal exact analysis max Rice partition order must be 0..8");
            }
            const auto frame_count = mono_plan_frame_count(plan);
            if (samples.empty()) {
                throw std::runtime_error("Metal analysis samples are empty");
            }
            std::optional<GeneratedLpcConfig> generated_config;
            std::vector<float> generated_windows;
            if (populate_lpc) {
                generated_config = generated_lpc_config(plan);
                generated_windows = make_generated_lpc_windows(
                    generated_config->blocksize,
                    generated_config->window_count,
                    plan.analysis_profile);
            }
            const auto lpc_order_limit = populate_lpc
                ? std::min<std::size_t>(
                    opencl_detail::kFlacClMaxOrder,
                    std::max<unsigned>(1U, plan.max_lpc_order))
                : 0U;
            const bool use_shifted_autocor =
                populate_lpc &&
                plan.analysis_profile == NativeAnalysisProfile::OrderGuessMeanEstimateRice &&
                generated_config->window_count == 3U &&
                generated_config->lpc_tasks_per_window == 1U &&
                generated_config->max_lpc_order <= 12U;
            const bool use_shifted_exact =
                use_shifted_autocor &&
                max_rice_partition_order <= kMetalExactLeafMaxRicePartitionOrder &&
                (generated_config->blocksize %
                    (std::size_t {1} << max_rice_partition_order)) == 0U &&
                (generated_config->blocksize >> max_rice_partition_order) >
                    generated_config->max_lpc_order;

            const auto samples_bytes =
                checked_buffer_bytes(samples.size(), sizeof(std::int32_t), "samples");
            const auto tasks_bytes = checked_buffer_bytes(
                plan.residual_tasks.size(), sizeof(FlacClSubframeTask), "tasks");
            const auto selected_bytes = checked_buffer_bytes(
                plan.selected_tasks.size(), sizeof(std::int32_t), "selectedTasks");
            const auto windows_bytes = populate_lpc
                ? checked_buffer_bytes(generated_windows.size(), sizeof(float), "windows")
                : 0;
            const auto autocor_bytes = populate_lpc
                ? checked_buffer_bytes(
                    generated_config->frame_count *
                        generated_config->window_count *
                        (opencl_detail::kFlacClMaxOrder + 1U),
                    sizeof(float),
                    "autocorrelations")
                : 0;
            const auto lpcs_bytes = populate_lpc
                ? checked_buffer_bytes(
                    generated_config->frame_count *
                        generated_config->window_count *
                        (opencl_detail::kFlacClMaxOrder + 1U) *
                        opencl_detail::kFlacClMaxOrder,
                    sizeof(float),
                    "LPC coefficients")
                : 0;
            const auto task_rice_bytes = checked_buffer_bytes(
                plan.residual_tasks.size(),
                sizeof(FlacClRiceParameterSet),
                "taskRiceParameters");
            const auto best_bytes =
                checked_buffer_bytes(frame_count, sizeof(FlacClSubframeTask), "bestTasks");
            const auto best_rice_bytes =
                checked_buffer_bytes(frame_count, sizeof(FlacClRiceParameterSet),
                    "bestRiceParameters");

            samples_buffer_ = ensure_buffer(device_, samples_buffer_, samples_buffer_bytes_,
                samples_bytes, "samples");
            tasks_buffer_ = ensure_buffer(device_, tasks_buffer_, tasks_buffer_bytes_,
                tasks_bytes, "tasks");
            selected_buffer_ = ensure_buffer(device_, selected_buffer_, selected_buffer_bytes_,
                selected_bytes, "selectedTasks");
            if (populate_lpc) {
                windows_buffer_ = ensure_buffer(device_, windows_buffer_, windows_buffer_bytes_,
                    windows_bytes, "windows");
                autocor_buffer_ = ensure_buffer(device_, autocor_buffer_, autocor_buffer_bytes_,
                    autocor_bytes, "autocorrelations");
                lpcs_buffer_ = ensure_buffer(device_, lpcs_buffer_, lpcs_buffer_bytes_,
                    lpcs_bytes, "LPC coefficients");
                if (use_shifted_autocor) {
                    shifted_samples_buffer_ = ensure_buffer(
                        device_,
                        shifted_samples_buffer_,
                        shifted_samples_buffer_bytes_,
                        samples_bytes,
                        "shifted samples");
                }
            }
            task_rice_parameters_buffer_ = ensure_buffer(device_, task_rice_parameters_buffer_,
                task_rice_parameters_buffer_bytes_, task_rice_bytes,
                "taskRiceParameters");
            best_buffer_ = ensure_buffer(device_, best_buffer_, best_buffer_bytes_,
                best_bytes, "bestTasks");
            best_rice_parameters_buffer_ = ensure_buffer(device_, best_rice_parameters_buffer_,
                best_rice_parameters_buffer_bytes_, best_rice_bytes,
                "bestRiceParameters");

            const auto upload_started = Clock::now();
            copy_to_buffer(samples_buffer_, samples.data(), samples_bytes);
            copy_to_buffer(tasks_buffer_, plan.residual_tasks.data(), tasks_bytes);
            copy_to_buffer(selected_buffer_, plan.selected_tasks.data(), selected_bytes);
            if (populate_lpc) {
                copy_to_buffer(windows_buffer_, generated_windows.data(), windows_bytes);
            }
            if (timings != nullptr) {
                add_elapsed_ns(timings->upload_ns, upload_started);
            }

            std::optional<GeneratedLpcParams> generated_params;
            if (populate_lpc) {
                generated_params = GeneratedLpcParams {
                    .frame_count = checked_u32(generated_config->frame_count, "frame count"),
                    .tasks_per_frame =
                        checked_u32(generated_config->tasks_per_frame, "tasks per frame"),
                    .lpc_tasks_per_window = checked_u32(
                        generated_config->lpc_tasks_per_window, "LPC tasks per window"),
                    .total_lpc_tasks =
                        checked_u32(generated_config->total_lpc_tasks, "total LPC tasks"),
                    .window_count =
                        checked_u32(generated_config->window_count, "generated LPC windows"),
                    .max_lpc_order =
                        checked_u32(generated_config->max_lpc_order, "max LPC order"),
                    .coefficient_precision =
                        checked_u32(lpc_coefficient_precision, "LPC coefficient precision"),
                    .analysis_profile = metal_analysis_profile_arg(plan.analysis_profile),
                    .blocksize = checked_u32(generated_config->blocksize, "block size"),
                    .fixed_order_guess_on_gpu = plan.fixed_order_guess_on_gpu ? 1U : 0U,
                    .shifted_samples_ready = use_shifted_autocor ? 1U : 0U,
                    .reserved1 = 0,
                };

                auto make_generated_command = [&]() {
                    id<MTLCommandBuffer> command = [queue_ commandBuffer];
                    if (command == nil) {
                        throw std::runtime_error(
                            "failed to create Metal generated-LPC command buffer");
                    }
                    return command;
                };
                auto finish_generated_command =
                    [&](id<MTLCommandBuffer> command,
                        const char* label,
                        Clock::time_point started,
                        std::uint64_t MetalGpuTimingStats::*counter) {
                        [command commit];
                        [command waitUntilCompleted];
                        if ([command status] != MTLCommandBufferStatusCompleted) {
                            throw std::runtime_error(
                                std::string(label) + " failed: " +
                                ns_error_text([command error]));
                        }
                        if (timings != nullptr && counter != nullptr) {
                            add_elapsed_ns(timings->*counter, started);
                        }
                    };
                auto encode_wasted = [&](id<MTLCommandBuffer> command) {
                    id<MTLComputeCommandEncoder> wasted_encoder =
                        [command computeCommandEncoder];
                    if (wasted_encoder == nil) {
                        throw std::runtime_error("failed to create Metal wasted-bits encoder");
                    }
                    [wasted_encoder setComputePipelineState:use_shifted_autocor
                            ? prepare_shifted_pipeline_
                            : find_wasted_pipeline_];
                    [wasted_encoder setBuffer:samples_buffer_ offset:0 atIndex:0];
                    [wasted_encoder setBuffer:tasks_buffer_ offset:0 atIndex:1];
                    [wasted_encoder setBytes:&(*generated_params)
                                      length:sizeof(*generated_params)
                                     atIndex:2];
                    if (use_shifted_autocor) {
                        [wasted_encoder setBuffer:shifted_samples_buffer_ offset:0 atIndex:3];
                    }
                    [wasted_encoder dispatchThreadgroups:MTLSizeMake(
                                                            generated_config->frame_count, 1, 1)
                                     threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
                    [wasted_encoder endEncoding];
                };
                auto encode_autocor = [&](id<MTLCommandBuffer> command) {
                    id<MTLComputeCommandEncoder> autocor_encoder =
                        [command computeCommandEncoder];
                    if (autocor_encoder == nil) {
                        throw std::runtime_error(
                            "failed to create Metal autocorrelation encoder");
                    }
                    [autocor_encoder setComputePipelineState:use_shifted_autocor
                            ? autocor_shifted_pipeline_
                            : autocor_pipeline_];
                    [autocor_encoder setBuffer:autocor_buffer_ offset:0 atIndex:0];
                    id<MTLBuffer> autocor_samples_buffer =
                        use_shifted_autocor ? shifted_samples_buffer_ : samples_buffer_;
                    [autocor_encoder setBuffer:autocor_samples_buffer offset:0 atIndex:1];
                    [autocor_encoder setBuffer:windows_buffer_ offset:0 atIndex:2];
                    [autocor_encoder setBuffer:tasks_buffer_ offset:0 atIndex:3];
                    [autocor_encoder setBytes:&(*generated_params)
                                       length:sizeof(*generated_params)
                                      atIndex:4];
                    [autocor_encoder dispatchThreadgroups:MTLSizeMake(
                                                            generated_config->frame_count,
                                                            generated_config->window_count,
                                                            lpc_order_limit + 1U)
                                     threadsPerThreadgroup:MTLSizeMake(
                                                                kWorkgroupSize, 1, 1)];
                    [autocor_encoder endEncoding];
                };
                auto encode_lpc = [&](id<MTLCommandBuffer> command) {
                    id<MTLComputeCommandEncoder> lpc_encoder =
                        [command computeCommandEncoder];
                    if (lpc_encoder == nil) {
                        throw std::runtime_error("failed to create Metal LPC encoder");
                    }
                    [lpc_encoder setComputePipelineState:lpc_pipeline_];
                    [lpc_encoder setBuffer:autocor_buffer_ offset:0 atIndex:0];
                    [lpc_encoder setBuffer:lpcs_buffer_ offset:0 atIndex:1];
                    [lpc_encoder setBytes:&(*generated_params)
                                    length:sizeof(*generated_params)
                                   atIndex:2];
                    [lpc_encoder dispatchThreads:MTLSizeMake(
                                                    generated_config->frame_count,
                                                    generated_config->window_count,
                                                    1)
                             threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                    [lpc_encoder endEncoding];
                };
                auto encode_quantize = [&](id<MTLCommandBuffer> command) {
                    id<MTLComputeCommandEncoder> quantize_encoder =
                        [command computeCommandEncoder];
                    if (quantize_encoder == nil) {
                        throw std::runtime_error("failed to create Metal quantize encoder");
                    }
                    [quantize_encoder setComputePipelineState:quantize_pipeline_];
                    [quantize_encoder setBuffer:tasks_buffer_ offset:0 atIndex:0];
                    [quantize_encoder setBuffer:lpcs_buffer_ offset:0 atIndex:1];
                    [quantize_encoder setBytes:&(*generated_params)
                                        length:sizeof(*generated_params)
                                       atIndex:2];
                    [quantize_encoder dispatchThreads:MTLSizeMake(
                                                         generated_config->frame_count,
                                                         generated_config->window_count,
                                                         1)
                                  threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                    [quantize_encoder endEncoding];
                };
                auto encode_fixed_guess = [&](id<MTLCommandBuffer> command) {
                    id<MTLComputeCommandEncoder> fixed_encoder =
                        [command computeCommandEncoder];
                    if (fixed_encoder == nil) {
                        throw std::runtime_error(
                            "failed to create Metal fixed-order guess encoder");
                    }
                    [fixed_encoder setComputePipelineState:fixed_guess_pipeline_];
                    [fixed_encoder setBuffer:samples_buffer_ offset:0 atIndex:0];
                    [fixed_encoder setBuffer:selected_buffer_ offset:0 atIndex:1];
                    [fixed_encoder setBuffer:tasks_buffer_ offset:0 atIndex:2];
                    [fixed_encoder setBytes:&(*generated_params)
                                      length:sizeof(*generated_params)
                                     atIndex:3];
                    [fixed_encoder setBuffer:(use_shifted_autocor
                            ? shifted_samples_buffer_
                            : samples_buffer_)
                                      offset:0
                                     atIndex:4];
                    [fixed_encoder dispatchThreadgroups:MTLSizeMake(
                                                            generated_config->frame_count, 1, 1)
                                     threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
                    [fixed_encoder endEncoding];
                };

                const auto generated_started = Clock::now();
                if (timings == nullptr) {
                    id<MTLCommandBuffer> generated_command = make_generated_command();
                    encode_wasted(generated_command);
                    encode_autocor(generated_command);
                    encode_lpc(generated_command);
                    encode_quantize(generated_command);
                    if (plan.fixed_order_guess_on_gpu) {
                        encode_fixed_guess(generated_command);
                    }
                    finish_generated_command(
                        generated_command,
                        "Metal generated LPC command",
                        generated_started,
                        nullptr);
                } else {
                    auto command = make_generated_command();
                    auto started = Clock::now();
                    encode_wasted(command);
                    finish_generated_command(
                        command,
                        "Metal wasted-bits command",
                        started,
                        &MetalGpuTimingStats::wasted_bits_ns);

                    command = make_generated_command();
                    started = Clock::now();
                    encode_autocor(command);
                    finish_generated_command(
                        command,
                        "Metal autocorrelation command",
                        started,
                        &MetalGpuTimingStats::generated_autocorrelation_ns);

                    command = make_generated_command();
                    started = Clock::now();
                    encode_lpc(command);
                    finish_generated_command(
                        command,
                        "Metal LPC command",
                        started,
                        &MetalGpuTimingStats::generated_lpc_ns);

                    command = make_generated_command();
                    started = Clock::now();
                    encode_quantize(command);
                    finish_generated_command(
                        command,
                        "Metal quantize command",
                        started,
                        &MetalGpuTimingStats::generated_quantize_ns);

                    if (plan.fixed_order_guess_on_gpu) {
                        command = make_generated_command();
                        started = Clock::now();
                        encode_fixed_guess(command);
                        finish_generated_command(
                            command,
                            "Metal fixed-order guess command",
                            started,
                            &MetalGpuTimingStats::fixed_order_guess_ns);
                    }
                    add_elapsed_ns(timings->generated_total_ns, generated_started);
                }
            }

            ExactParams exact_params {
                .selected_task_count = checked_u32(plan.selected_tasks.size(), "selected tasks"),
                .max_rice_partition_order = max_rice_partition_order,
                .analysis_profile = metal_analysis_profile_arg(plan.analysis_profile),
                .prepared_frame_facts = populate_lpc ? 1U : 0U,
            };
            ChooseParams choose_params {
                .frame_count = checked_u32(frame_count, "frame count"),
                .tasks_per_frame = checked_u32(plan.estimate_tasks_per_frame, "tasks per frame"),
                .reserved0 = 0,
                .reserved1 = 0,
            };

            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) {
                throw std::runtime_error("failed to create Metal command buffer");
            }

            const auto exact_started = Clock::now();
            id<MTLComputeCommandEncoder> exact_encoder = [command computeCommandEncoder];
            if (exact_encoder == nil) {
                throw std::runtime_error("failed to create Metal exact-analysis encoder");
            }
            [exact_encoder setComputePipelineState:use_shifted_exact
                    ? exact_shifted_pipeline_
                    : exact_pipeline_];
            [exact_encoder setBuffer:samples_buffer_ offset:0 atIndex:0];
            [exact_encoder setBuffer:selected_buffer_ offset:0 atIndex:1];
            [exact_encoder setBuffer:tasks_buffer_ offset:0 atIndex:2];
            [exact_encoder setBuffer:task_rice_parameters_buffer_ offset:0 atIndex:3];
            [exact_encoder setBytes:&exact_params length:sizeof(exact_params) atIndex:4];
            if (use_shifted_exact) {
                [exact_encoder setBuffer:shifted_samples_buffer_ offset:0 atIndex:5];
            }
            [exact_encoder dispatchThreadgroups:MTLSizeMake(plan.selected_tasks.size(), 1, 1)
                          threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
            [exact_encoder endEncoding];

            id<MTLComputeCommandEncoder> choose_encoder = [command computeCommandEncoder];
            if (choose_encoder == nil) {
                throw std::runtime_error("failed to create Metal choose-best encoder");
            }
            [choose_encoder setComputePipelineState:choose_pipeline_];
            [choose_encoder setBuffer:selected_buffer_ offset:0 atIndex:0];
            [choose_encoder setBuffer:tasks_buffer_ offset:0 atIndex:1];
            [choose_encoder setBuffer:task_rice_parameters_buffer_ offset:0 atIndex:2];
            [choose_encoder setBuffer:best_buffer_ offset:0 atIndex:3];
            [choose_encoder setBuffer:best_rice_parameters_buffer_ offset:0 atIndex:4];
            [choose_encoder setBytes:&choose_params length:sizeof(choose_params) atIndex:5];
            [choose_encoder dispatchThreads:MTLSizeMake(frame_count, 1, 1)
                       threadsPerThreadgroup:MTLSizeMake(kWorkgroupSize, 1, 1)];
            [choose_encoder endEncoding];

            [command commit];
            [command waitUntilCompleted];
            if ([command status] != MTLCommandBufferStatusCompleted) {
                throw std::runtime_error("Metal analysis command failed: " +
                    ns_error_text([command error]));
            }
            if (timings != nullptr) {
                add_elapsed_ns(timings->exact_analysis_ns, exact_started);
                ++timings->batches;
            }

            const auto readback_started = Clock::now();
            std::vector<FlacClSubframeTask> analyzed_tasks;
            if (read_analyzed_tasks) {
                analyzed_tasks.resize(plan.residual_tasks.size());
                copy_from_buffer(analyzed_tasks.data(), tasks_buffer_, tasks_bytes);
            }
            std::vector<FlacClSubframeTask> best_tasks(frame_count);
            std::vector<FlacClRiceParameterSet> best_rice_parameters(frame_count);
            copy_from_buffer(best_tasks.data(), best_buffer_, best_bytes);
            copy_from_buffer(best_rice_parameters.data(),
                best_rice_parameters_buffer_, best_rice_bytes);
            if (timings != nullptr) {
                add_elapsed_ns(timings->readback_ns, readback_started);
            }

            return OpenClMonoFixedConstantAnalysisResult {
                .analyzed_tasks = std::move(analyzed_tasks),
                .best_tasks = std::move(best_tasks),
                .best_rice_parameters = std::move(best_rice_parameters),
                .device_name = device_name_,
            };
        }
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> find_wasted_pipeline_ = nil;
    id<MTLComputePipelineState> prepare_shifted_pipeline_ = nil;
    id<MTLComputePipelineState> autocor_pipeline_ = nil;
    id<MTLComputePipelineState> autocor_shifted_pipeline_ = nil;
    id<MTLComputePipelineState> lpc_pipeline_ = nil;
    id<MTLComputePipelineState> quantize_pipeline_ = nil;
    id<MTLComputePipelineState> fixed_guess_pipeline_ = nil;
    id<MTLComputePipelineState> exact_pipeline_ = nil;
    id<MTLComputePipelineState> exact_shifted_pipeline_ = nil;
    id<MTLComputePipelineState> choose_pipeline_ = nil;
    id<MTLBuffer> samples_buffer_ = nil;
    id<MTLBuffer> shifted_samples_buffer_ = nil;
    id<MTLBuffer> tasks_buffer_ = nil;
    id<MTLBuffer> selected_buffer_ = nil;
    id<MTLBuffer> windows_buffer_ = nil;
    id<MTLBuffer> autocor_buffer_ = nil;
    id<MTLBuffer> lpcs_buffer_ = nil;
    id<MTLBuffer> task_rice_parameters_buffer_ = nil;
    id<MTLBuffer> best_buffer_ = nil;
    id<MTLBuffer> best_rice_parameters_buffer_ = nil;
    std::size_t samples_buffer_bytes_ = 0;
    std::size_t shifted_samples_buffer_bytes_ = 0;
    std::size_t tasks_buffer_bytes_ = 0;
    std::size_t selected_buffer_bytes_ = 0;
    std::size_t windows_buffer_bytes_ = 0;
    std::size_t autocor_buffer_bytes_ = 0;
    std::size_t lpcs_buffer_bytes_ = 0;
    std::size_t task_rice_parameters_buffer_bytes_ = 0;
    std::size_t best_buffer_bytes_ = 0;
    std::size_t best_rice_parameters_buffer_bytes_ = 0;
    std::string device_name_;
};

MetalMonoAnalysisSession::MetalMonoAnalysisSession(
    std::optional<std::size_t> requested_device_index)
    : impl_(std::make_unique<Impl>(requested_device_index))
{
}

MetalMonoAnalysisSession::~MetalMonoAnalysisSession() = default;

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, true, nullptr);
}

OpenClMonoBestMethodResult MetalMonoAnalysisSession::run_fixed_constant_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    auto analysis = impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, false, gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
}

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, 12, max_rice_partition_order, false, true, nullptr);
}

OpenClMonoFixedConstantAnalysisResult MetalMonoAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    return impl_->run_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order,
        true, true, nullptr);
}

OpenClMonoBestMethodResult MetalMonoAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    auto analysis = impl_->run_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order,
        true, false, gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_fixed_constant_analysis(samples, plan, max_rice_partition_order);
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_lpc_analysis(samples, plan, max_rice_partition_order);
}

OpenClMonoFixedConstantAnalysisResult run_metal_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    MetalMonoAnalysisSession session(requested_device_index);
    return session.run_generated_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
}

}  // namespace ldcompress::metal_detail
