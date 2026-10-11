#include "native_flac_encoder.h"

#include "flac_native_writer.h"
#include "hash.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxNativeFrameSamples = 4608;
constexpr unsigned kMaxNativeLpcOrder = 12;
constexpr unsigned kMinNativeLpcPrecision = 1;
constexpr unsigned kMaxNativeLpcPrecision = 15;
constexpr unsigned kMaxNativeRicePartitionOrder = 8;

enum class NativeFrameCoding {
    Verbatim,
    FixedRice,
};

void update_md5_s16le(Md5& md5, const SampleGroup& samples)
{
    std::array<std::uint8_t, 8> bytes {};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(samples[i]);
        bytes[(i * 2U) + 0U] = static_cast<std::uint8_t>(value & 0xffU);
        bytes[(i * 2U) + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    }
    md5.update(bytes.data(), bytes.size());
}

template <typename OnGroup>
ConversionStats process_lds_sample_groups(
    std::istream& input,
    OnGroup&& on_group,
    const CompressionProgressCallback& progress_callback)
{
    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);

    while (input) {
        input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        for (std::size_t i = 0; i < groups; ++i) {
            PackedLdsGroup packed;
            std::memcpy(packed.data(), input_buffer.data() + (i * 5), packed.size());
            const SampleGroup samples = unpack_group(packed);
            on_group(samples);
        }

        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4;
        if (progress_callback) {
            progress_callback(stats.input_bytes, stats.samples);
        }
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    return stats;
}

void rewrite_native_flac_streaminfo(
    std::ostream& output,
    const FlacStreamInfo& streaminfo,
    const std::string& output_path)
{
    output.seekp(0, std::ios::beg);
    if (!output) {
        throw std::runtime_error(
            "failed to seek native FLAC output for STREAMINFO rewrite: " + output_path);
    }
    write_native_flac_streaminfo(output, streaminfo);
}

FlacStreamInfo make_streaminfo(
    const ConversionStats& stats,
    unsigned frame_sample_count,
    unsigned sample_rate,
    const std::array<std::uint8_t, 16>& md5)
{
    auto streaminfo_block_size = frame_sample_count;
    if (stats.samples == 0) {
        streaminfo_block_size = kMinimumStreamInfoBlockSize;
    } else if (stats.samples < frame_sample_count) {
        streaminfo_block_size = static_cast<unsigned>(
            std::max<std::uint64_t>(stats.samples, kMinimumStreamInfoBlockSize));
    }

    return FlacStreamInfo {
        .min_block_size = streaminfo_block_size,
        .max_block_size = streaminfo_block_size,
        .min_frame_size = 0,
        .max_frame_size = 0,
        .sample_rate = sample_rate,
        .channels = 1,
        .bits_per_sample = 16,
        .total_samples = flac_streaminfo_total_samples_or_unknown(stats.samples),
        .md5 = md5,
    };
}

void record_native_stats(
    NativeCompressionStats* stats,
    const FlacSubframeDecision& decision)
{
    if (stats == nullptr) {
        return;
    }

    ++stats->frames;
    stats->estimated_subframe_bits += decision.estimated_bits;
    if (decision.wasted_bits < stats->wasted_bits_counts.size()) {
        ++stats->wasted_bits_counts[decision.wasted_bits];
    }

    switch (decision.kind) {
    case FlacSubframeKind::Constant:
        ++stats->constant_frames;
        return;
    case FlacSubframeKind::Verbatim:
        ++stats->verbatim_frames;
        return;
    case FlacSubframeKind::FixedRice:
        ++stats->fixed_rice_frames;
        if (decision.fixed_order < stats->fixed_order_counts.size()) {
            ++stats->fixed_order_counts[decision.fixed_order];
        }
        if (decision.rice_partition_order < stats->partition_order_counts.size()) {
            ++stats->partition_order_counts[decision.rice_partition_order];
        }
        return;
    case FlacSubframeKind::LpcRice:
        ++stats->lpc_rice_frames;
        if (decision.lpc_order < stats->lpc_order_counts.size()) {
            ++stats->lpc_order_counts[decision.lpc_order];
        }
        if (decision.rice_partition_order < stats->partition_order_counts.size()) {
            ++stats->partition_order_counts[decision.rice_partition_order];
        }
        return;
    }

    throw std::runtime_error("unknown native FLAC subframe kind");
}

FlacSubframeDecision write_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    std::uint64_t frame_number,
    unsigned sample_rate,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    NativeFrameCoding coding,
    NativeAnalysisProfile analysis_profile)
{
    const FlacFrameInfo frame_info {
        .frame_number = frame_number,
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
        .max_lpc_order = max_lpc_order,
        .lpc_coefficient_precision = lpc_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
    switch (coding) {
    case NativeFrameCoding::Verbatim:
        return write_mono_verbatim_frame(output, samples, frame_info);
    case NativeFrameCoding::FixedRice:
        return write_mono_best_frame(output, samples, frame_info, analysis_profile);
    }
    throw std::runtime_error("unknown native FLAC frame coding");
}

struct EncodedFrame {
    std::string bytes;
    FlacSubframeDecision decision;
};

struct FrameJob {
    std::uint64_t frame_number = 0;
    std::vector<std::int32_t> samples;
};

EncodedFrame encode_frame(
    std::vector<std::int32_t> samples,
    std::uint64_t frame_number,
    unsigned sample_rate,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    NativeFrameCoding coding,
    NativeAnalysisProfile analysis_profile)
{
    std::ostringstream output(std::ios::out | std::ios::binary);
    const auto decision = write_frame(
        output, samples, frame_number, sample_rate, max_lpc_order,
        lpc_precision, max_rice_partition_order, coding, analysis_profile);
    if (!output) {
        throw std::runtime_error("failed to encode native FLAC frame");
    }
    return EncodedFrame {
        .bytes = output.str(),
        .decision = decision,
    };
}

void write_frame_bytes(std::ostream& output, const std::string& bytes)
{
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC frame output");
    }
}

class FrameEncoderPool final {
public:
    FrameEncoderPool(
        unsigned thread_count,
        unsigned sample_rate,
        unsigned max_lpc_order,
        unsigned lpc_precision,
        unsigned max_rice_partition_order,
        NativeFrameCoding coding,
        NativeAnalysisProfile analysis_profile)
        : sample_rate_(sample_rate),
          max_lpc_order_(max_lpc_order),
          lpc_precision_(lpc_precision),
          max_rice_partition_order_(max_rice_partition_order),
          coding_(coding),
          analysis_profile_(analysis_profile)
    {
        workers_.reserve(thread_count);
        for (unsigned i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] {
                worker_loop();
            });
        }
    }

    FrameEncoderPool(const FrameEncoderPool&) = delete;
    FrameEncoderPool& operator=(const FrameEncoderPool&) = delete;

    ~FrameEncoderPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        jobs_available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void submit(std::uint64_t frame_number, std::vector<std::int32_t> samples)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rethrow_exception_if_present();
            jobs_.push_back(FrameJob {
                .frame_number = frame_number,
                .samples = std::move(samples),
            });
            ++in_flight_;
        }
        jobs_available_.notify_one();
    }

    std::size_t pending_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_flight_;
    }

    EncodedFrame take(std::uint64_t frame_number)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        results_available_.wait(lock, [&] {
            return exception_ != nullptr || results_.find(frame_number) != results_.end();
        });
        rethrow_exception_if_present();

        auto result = std::move(results_.at(frame_number));
        results_.erase(frame_number);
        --in_flight_;
        return result;
    }

private:
    void rethrow_exception_if_present() const
    {
        if (exception_ != nullptr) {
            std::rethrow_exception(exception_);
        }
    }

    void worker_loop()
    {
        while (true) {
            FrameJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                jobs_available_.wait(lock, [&] {
                    return stopping_ || !jobs_.empty();
                });
                if (stopping_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            try {
                auto frame = encode_frame(std::move(job.samples), job.frame_number,
                    sample_rate_, max_lpc_order_, lpc_precision_,
                    max_rice_partition_order_, coding_, analysis_profile_);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    results_.emplace(job.frame_number, std::move(frame));
                }
                results_available_.notify_all();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (exception_ == nullptr) {
                        exception_ = std::current_exception();
                    }
                    stopping_ = true;
                }
                jobs_available_.notify_all();
                results_available_.notify_all();
                return;
            }
        }
    }

    unsigned sample_rate_ = 0;
    unsigned max_lpc_order_ = 0;
    unsigned lpc_precision_ = 0;
    unsigned max_rice_partition_order_ = 0;
    NativeFrameCoding coding_ = NativeFrameCoding::Verbatim;
    NativeAnalysisProfile analysis_profile_ = NativeAnalysisProfile::Exact;
    mutable std::mutex mutex_;
    std::condition_variable jobs_available_;
    std::condition_variable results_available_;
    std::deque<FrameJob> jobs_;
    std::map<std::uint64_t, EncodedFrame> results_;
    std::vector<std::thread> workers_;
    std::exception_ptr exception_;
    bool stopping_ = false;
    std::size_t in_flight_ = 0;
};

}  // namespace

ConversionStats compress_lds_to_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    NativeFrameCoding coding,
    unsigned thread_count,
    unsigned frame_sample_count,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    NativeAnalysisProfile analysis_profile,
    NativeCompressionStats* native_stats,
    CompressionProgressCallback progress_callback)
{
    if (thread_count == 0) {
        throw std::runtime_error("native FLAC thread count must be at least 1");
    }
    if (frame_sample_count < kMinimumStreamInfoBlockSize ||
        frame_sample_count > kMaxNativeFrameSamples) {
        throw std::runtime_error("native FLAC frame sample count must be 16..4608");
    }
    if (max_lpc_order > kMaxNativeLpcOrder) {
        throw std::runtime_error("native FLAC max LPC order must be 0..12");
    }
    if (lpc_precision < kMinNativeLpcPrecision ||
        lpc_precision > kMaxNativeLpcPrecision) {
        throw std::runtime_error("native FLAC LPC coefficient precision must be 1..15");
    }
    if (max_rice_partition_order > kMaxNativeRicePartitionOrder) {
        throw std::runtime_error("native FLAC max Rice partition order must be 0..8");
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(
        output,
        make_streaminfo(
            ConversionStats {}, frame_sample_count, sample_rate,
            std::array<std::uint8_t, 16> {}));

    Md5 pcm_md5;
    std::vector<std::int32_t> frame_samples;
    frame_samples.reserve(frame_sample_count);
    std::uint64_t frame_number = 0;
    std::uint64_t next_frame_to_write = 0;
    std::unique_ptr<FrameEncoderPool> frame_pool;
    if (thread_count != 1) {
        frame_pool = std::make_unique<FrameEncoderPool>(
            thread_count, sample_rate, max_lpc_order, lpc_precision,
            max_rice_partition_order, coding, analysis_profile);
    }
    const auto max_pending_frames = static_cast<std::size_t>(thread_count) * 2U;

    const auto flush_next_threaded_frame = [&] {
        if (!frame_pool) {
            return;
        }
        const auto encoded_frame = frame_pool->take(next_frame_to_write);
        write_frame_bytes(output, encoded_frame.bytes);
        record_native_stats(native_stats, encoded_frame.decision);
        ++next_frame_to_write;
    };

    const auto submit_frame = [&](std::vector<std::int32_t> samples, std::uint64_t current_frame) {
        if (thread_count == 1) {
            const auto decision = write_frame(
                output, samples, current_frame, sample_rate, max_lpc_order,
                lpc_precision, max_rice_partition_order, coding, analysis_profile);
            record_native_stats(native_stats, decision);
            ++next_frame_to_write;
            return;
        }

        while (frame_pool->pending_count() >= max_pending_frames) {
            flush_next_threaded_frame();
        }
        frame_pool->submit(current_frame, std::move(samples));
    };

    auto stats = process_lds_sample_groups(
        lds_input,
        [&](const SampleGroup& samples) {
            update_md5_s16le(pcm_md5, samples);
            for (const auto sample : samples) {
                frame_samples.push_back(sample);
                if (frame_samples.size() == frame_sample_count) {
                    submit_frame(std::move(frame_samples), frame_number);
                    ++frame_number;
                    frame_samples.clear();
                    frame_samples.reserve(frame_sample_count);
                }
            }
        },
        progress_callback);
    if (!frame_samples.empty()) {
        submit_frame(std::move(frame_samples), frame_number);
        ++frame_number;
    }
    while (next_frame_to_write != frame_number) {
        flush_next_threaded_frame();
    }
    if (next_frame_to_write != frame_number) {
        throw std::runtime_error("internal native FLAC frame count mismatch");
    }

    const auto streaminfo = make_streaminfo(
        stats, frame_sample_count, sample_rate, pcm_md5.digest());
    rewrite_native_flac_streaminfo(output, streaminfo, output_path);

    output.close();
    if (!output) {
        throw std::runtime_error("failed to finish native FLAC output: " + output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

ConversionStats compress_lds_to_native_verbatim_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    unsigned frame_samples,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    NativeCompressionStats* stats,
    CompressionProgressCallback progress_callback)
{
    return compress_lds_to_native_flac(
        lds_input, output_path, sample_rate, NativeFrameCoding::Verbatim, thread_count,
        frame_samples, max_lpc_order, lpc_precision, max_rice_partition_order,
        NativeAnalysisProfile::Exact, stats, std::move(progress_callback));
}

ConversionStats compress_lds_to_native_fixed_flac(
    std::istream& lds_input,
    const std::string& output_path,
    unsigned sample_rate,
    unsigned thread_count,
    unsigned frame_samples,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    NativeAnalysisProfile analysis_profile,
    NativeCompressionStats* stats,
    CompressionProgressCallback progress_callback)
{
    return compress_lds_to_native_flac(
        lds_input, output_path, sample_rate, NativeFrameCoding::FixedRice, thread_count,
        frame_samples, max_lpc_order, lpc_precision, max_rice_partition_order,
        analysis_profile, stats, std::move(progress_callback));
}

}  // namespace ldcompress
