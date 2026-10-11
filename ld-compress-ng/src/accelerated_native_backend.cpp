#include "accelerated_native_backend.h"

#include "compressor.h"
#include "hash.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <span>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kGroupsPerChunk = 8192;
constexpr std::size_t kSamplesPerGroup = 4;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point finish)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += elapsed_ns(start, Clock::now());
}

void write_i16le(std::uint8_t* bytes, std::int16_t sample)
{
    const auto value = static_cast<std::uint16_t>(sample);
    bytes[0] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

SampleGroup unpack_group_bytes(const std::uint8_t* bytes)
{
    const int byte0 = bytes[0];
    const int byte1 = bytes[1];
    const int byte2 = bytes[2];
    const int byte3 = bytes[3];
    const int byte4 = bytes[4];

    const int word0 = ((byte0 & 0xff) * 4) + ((byte1 & 0xc0) >> 6);
    const int word1 = ((byte1 & 0x3f) * 16) + ((byte2 & 0xf0) >> 4);
    const int word2 = ((byte2 & 0x0f) * 64) + ((byte3 & 0xfc) >> 2);
    const int word3 = ((byte3 & 0x03) * 256) + (byte4 & 0xff);

    return {
        static_cast<std::int16_t>((word0 - 512) * 64),
        static_cast<std::int16_t>((word1 - 512) * 64),
        static_cast<std::int16_t>((word2 - 512) * 64),
        static_cast<std::int16_t>((word3 - 512) * 64),
    };
}

void rewrite_native_flac_streaminfo(
    std::ostream& output,
    const FlacStreamInfo& streaminfo,
    const std::string& output_path,
    const char* backend_label)
{
    output.seekp(0, std::ios::beg);
    if (!output) {
        throw std::runtime_error(
            "failed to seek " + std::string(backend_label) +
            " FLAC output for STREAMINFO rewrite: " + output_path);
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
        streaminfo_block_size = stats.samples < kMinimumStreamInfoBlockSize
            ? kMinimumStreamInfoBlockSize
            : static_cast<unsigned>(stats.samples);
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

void record_selected_write_timings(
    NativeCompressionStats* stats,
    const FlacSelectedFrameWriteTimings& timings)
{
    if (stats == nullptr) {
        return;
    }

    stats->accelerated_selected_validation_ns += timings.validation_wasted_ns;
    stats->accelerated_selected_shift_ns += timings.shift_ns;
    stats->accelerated_selected_residual_ns += timings.residual_ns;
    stats->accelerated_selected_rice_parameter_ns += timings.rice_parameter_ns;
    stats->accelerated_selected_bitstream_ns += timings.bitstream_ns;
    stats->accelerated_selected_frame_output_ns += timings.frame_output_ns;
}

struct SelectedEncodedFrame {
    std::string bytes;
    FlacSubframeDecision decision;
    FlacSelectedFrameWriteTimings timings;
};

struct SelectedFrameJob {
    std::uint64_t frame_number = 0;
    FlacFrameInfo frame_info;
    std::shared_ptr<const std::vector<std::int32_t>> sample_owner;
    std::size_t sample_offset = 0;
    std::size_t sample_count = 0;
    FlacSelectedSubframe selected;
    FlacSubframeDecision decision;
    bool collect_timings = false;
};

struct AnalyzedSelectedBatch {
    std::vector<std::int32_t> samples;
    std::uint64_t first_frame_number = 0;
    AcceleratedSelectedFrameAnalysis analysis;
};

class FrameByteStreambuf final : public std::streambuf {
public:
    explicit FrameByteStreambuf(std::size_t reserve_bytes)
    {
        bytes_.reserve(reserve_bytes);
    }

    std::string take() { return std::move(bytes_); }

protected:
    int_type overflow(int_type ch) override
    {
        if (std::char_traits<char>::eq_int_type(ch, std::char_traits<char>::eof())) {
            return std::char_traits<char>::not_eof(ch);
        }
        bytes_.push_back(std::char_traits<char>::to_char_type(ch));
        return ch;
    }

    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        if (size > 0) {
            bytes_.append(data, static_cast<std::size_t>(size));
        }
        return size;
    }

private:
    std::string bytes_;
};

FlacFrameInfo make_frame_info(
    std::uint64_t frame_number,
    const AcceleratedNativeCompressionOptions& options)
{
    return FlacFrameInfo {
        .frame_number = frame_number,
        .sample_rate = options.sample_rate,
        .bits_per_sample = 16,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
    };
}

SelectedEncodedFrame encode_selected_frame(SelectedFrameJob job)
{
    if (job.sample_owner == nullptr ||
        job.sample_offset > job.sample_owner->size() ||
        job.sample_count > job.sample_owner->size() - job.sample_offset) {
        throw std::runtime_error("selected native FLAC frame sample range is invalid");
    }
    const std::span<const std::int32_t> samples(
        job.sample_owner->data() + job.sample_offset,
        job.sample_count);
    const auto expected_bytes =
        static_cast<std::size_t>((job.decision.estimated_bits + 7U) / 8U) + 24U;
    FrameByteStreambuf output_buffer(expected_bytes);
    std::ostream output(&output_buffer);
    FlacSelectedFrameWriteTimings timings;
    const auto decision = write_mono_selected_frame_with_decision(
        output,
        samples,
        job.frame_info,
        job.selected,
        job.decision,
        job.collect_timings ? &timings : nullptr);
    if (!output) {
        throw std::runtime_error("failed to encode selected native FLAC frame");
    }
    return SelectedEncodedFrame {
        .bytes = output_buffer.take(),
        .decision = decision,
        .timings = timings,
    };
}

void write_frame_bytes(std::ostream& output, const std::string& bytes)
{
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write selected native FLAC frame output");
    }
}

class SelectedFrameWriterPool final {
public:
    explicit SelectedFrameWriterPool(unsigned thread_count)
    {
        workers_.reserve(thread_count);
        for (unsigned i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] {
                worker_loop();
            });
        }
    }

    SelectedFrameWriterPool(const SelectedFrameWriterPool&) = delete;
    SelectedFrameWriterPool& operator=(const SelectedFrameWriterPool&) = delete;

    ~SelectedFrameWriterPool()
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

    void submit(SelectedFrameJob job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rethrow_exception_if_present();
            jobs_.push_back(std::move(job));
        }
        jobs_available_.notify_one();
    }

    SelectedEncodedFrame take(std::uint64_t frame_number)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        results_available_.wait(lock, [&] {
            return exception_ != nullptr || results_.find(frame_number) != results_.end();
        });
        rethrow_exception_if_present();

        auto result = std::move(results_.at(frame_number));
        results_.erase(frame_number);
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
            SelectedFrameJob job;
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
                const auto frame_number = job.frame_number;
                auto frame = encode_selected_frame(std::move(job));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    results_.emplace(frame_number, std::move(frame));
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

    mutable std::mutex mutex_;
    std::condition_variable jobs_available_;
    std::condition_variable results_available_;
    std::deque<SelectedFrameJob> jobs_;
    std::map<std::uint64_t, SelectedEncodedFrame> results_;
    std::vector<std::thread> workers_;
    std::exception_ptr exception_;
    bool stopping_ = false;
};

AnalyzedSelectedBatch analyze_accelerated_selected_batch(
    std::vector<std::int32_t> batch_samples,
    std::uint64_t first_frame_number,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer)
{
    if (batch_samples.empty()) {
        return AnalyzedSelectedBatch {};
    }
    if ((batch_samples.size() % options.frame_samples) != 0) {
        throw std::runtime_error(
            "internal " + std::string(options.backend_label) +
            " batch was not frame-aligned");
    }

    const auto frame_count = batch_samples.size() / options.frame_samples;
    auto base_frame_info = make_frame_info(first_frame_number, options);
    if (options.native_stats != nullptr) {
        ++options.native_stats->accelerated_batches;
    }
    const auto analyzer_started = Clock::now();
    auto analysis = analyzer(batch_samples, base_frame_info, options.frame_samples);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_analyzer_ns, analyzer_started);
    }
    if (analysis.selected_subframes.size() != frame_count ||
        analysis.decisions.size() != frame_count) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " selected frame count did not match input batch");
    }

    return AnalyzedSelectedBatch {
        .samples = std::move(batch_samples),
        .first_frame_number = first_frame_number,
        .analysis = std::move(analysis),
    };
}

void write_accelerated_analyzed_batch(
    std::ostream& output,
    AnalyzedSelectedBatch batch,
    const AcceleratedNativeCompressionOptions& options,
    SelectedFrameWriterPool* frame_pool)
{
    if (batch.samples.empty()) {
        return;
    }
    if ((batch.samples.size() % options.frame_samples) != 0) {
        throw std::runtime_error(
            "internal " + std::string(options.backend_label) +
            " analyzed batch was not frame-aligned");
    }

    const auto frame_count = batch.samples.size() / options.frame_samples;
    if (batch.analysis.selected_subframes.size() != frame_count ||
        batch.analysis.decisions.size() != frame_count) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " analyzed frame count did not match input batch");
    }

    std::shared_ptr<const std::vector<std::int32_t>> threaded_sample_owner;
    if (frame_pool != nullptr) {
        threaded_sample_owner =
            std::make_shared<const std::vector<std::int32_t>>(std::move(batch.samples));
    }
    const auto& samples = frame_pool != nullptr
        ? *threaded_sample_owner
        : batch.samples;

    const auto write_started = Clock::now();
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::span<const std::int32_t> frame_samples(
            samples.data() + offset,
            static_cast<std::size_t>(options.frame_samples));
        auto frame_info = make_frame_info(batch.first_frame_number + frame, options);
        if (frame_pool != nullptr) {
            frame_pool->submit(SelectedFrameJob {
                .frame_number = batch.first_frame_number + frame,
                .frame_info = frame_info,
                .sample_owner = threaded_sample_owner,
                .sample_offset = offset,
                .sample_count = static_cast<std::size_t>(options.frame_samples),
                .selected = batch.analysis.selected_subframes[frame],
                .decision = batch.analysis.decisions[frame],
                .collect_timings = options.native_stats != nullptr,
            });
            continue;
        }
        FlacSelectedFrameWriteTimings writer_timings;
        const auto decision = write_mono_selected_frame_with_decision(
            output,
            frame_samples,
            frame_info,
            batch.analysis.selected_subframes[frame],
            batch.analysis.decisions[frame],
            options.native_stats != nullptr ? &writer_timings : nullptr);
        record_native_stats(options.native_stats, decision);
        record_selected_write_timings(options.native_stats, writer_timings);
    }
    if (frame_pool != nullptr) {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            auto encoded = frame_pool->take(batch.first_frame_number + frame);
            write_frame_bytes(output, encoded.bytes);
            record_native_stats(options.native_stats, encoded.decision);
            record_selected_write_timings(options.native_stats, encoded.timings);
        }
    }
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_selected_write_ns, write_started);
    }
}

void write_native_tail_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    std::uint64_t frame_number,
    const AcceleratedNativeCompressionOptions& options)
{
    if (samples.empty()) {
        return;
    }

    const auto write_started = Clock::now();
    const auto decision = write_mono_best_frame(
        output, samples, make_frame_info(frame_number, options));
    record_native_stats(options.native_stats, decision);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_tail_write_ns, write_started);
    }
}

}  // namespace

ConversionStats compress_lds_to_accelerated_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const AcceleratedNativeCompressionOptions& options,
    const AcceleratedBatchAnalyzer& analyzer)
{
    if (options.thread_count == 0) {
        throw std::runtime_error(
            std::string(options.backend_label) +
            " FLAC thread count must be at least 1");
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not open output: " + output_path);
    }

    write_native_flac_streaminfo(
        output,
        make_streaminfo(
            ConversionStats {}, options.frame_samples, options.sample_rate,
            std::array<std::uint8_t, 16> {}));

    Md5 pcm_md5;

    std::vector<std::int32_t> batch_samples;
    const auto frame_sample_count = static_cast<std::size_t>(options.frame_samples);
    const auto batch_sample_count = frame_sample_count * options.batch_frames;
    batch_samples.reserve(batch_sample_count);
    std::future<AnalyzedSelectedBatch> pending_batch;
    std::uint64_t frame_number = 0;
    std::size_t samples_in_current_frame = 0;
    std::unique_ptr<SelectedFrameWriterPool> frame_pool;
    if (options.thread_count != 1) {
        frame_pool = std::make_unique<SelectedFrameWriterPool>(options.thread_count);
    }

    const auto launch_batch_analysis = [&](std::vector<std::int32_t> samples,
                                           std::uint64_t first_frame_number) {
        return std::async(
            std::launch::async,
            [&options, &analyzer,
                samples = std::move(samples), first_frame_number]() mutable {
                return analyze_accelerated_selected_batch(
                    std::move(samples), first_frame_number, options, analyzer);
            });
    };
    const auto write_pending_batch = [&] {
        if (!pending_batch.valid()) {
            return;
        }
        auto analyzed = pending_batch.get();
        write_accelerated_analyzed_batch(
            output, std::move(analyzed), options, frame_pool.get());
    };
    const auto flush_batch = [&] {
        if (batch_samples.empty()) {
            return;
        }
        const auto batch_first_frame =
            frame_number - (batch_samples.size() / frame_sample_count);
        std::vector<std::int32_t> full_batch;
        full_batch.swap(batch_samples);
        batch_samples.reserve(batch_sample_count);
        if (pending_batch.valid()) {
            auto analyzed = pending_batch.get();
            pending_batch = launch_batch_analysis(std::move(full_batch), batch_first_frame);
            write_accelerated_analyzed_batch(
                output, std::move(analyzed), options, frame_pool.get());
            return;
        }
        pending_batch = launch_batch_analysis(std::move(full_batch), batch_first_frame);
    };

    const auto ingest_started = Clock::now();
    const auto analyzer_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_analyzer_ns
        : 0;
    const auto selected_write_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_selected_write_ns
        : 0;
    const auto tail_write_before = options.native_stats != nullptr
        ? options.native_stats->accelerated_tail_write_ns
        : 0;
    std::uint64_t explicit_scan_ns = 0;
    std::uint64_t scan_read_ns = 0;
    std::uint64_t scan_decode_ns = 0;
    std::uint64_t scan_md5_ns = 0;

    const auto append_sample = [&](std::int16_t sample) {
        batch_samples.push_back(sample);
        ++samples_in_current_frame;
        if (samples_in_current_frame == frame_sample_count) {
            samples_in_current_frame = 0;
            ++frame_number;
            if (batch_samples.size() == batch_sample_count) {
                flush_batch();
            }
        }
    };
    const bool frame_is_group_aligned = (frame_sample_count % kSamplesPerGroup) == 0;
    const auto append_sample_group = [&](const SampleGroup& samples) {
        if (!frame_is_group_aligned) {
            for (const auto sample : samples) {
                append_sample(sample);
            }
            return false;
        }

        const auto old_size = batch_samples.size();
        batch_samples.resize(old_size + samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            batch_samples[old_size + i] = samples[i];
        }

        samples_in_current_frame += samples.size();
        if (samples_in_current_frame == frame_sample_count) {
            samples_in_current_frame = 0;
            ++frame_number;
            return batch_samples.size() == batch_sample_count;
        }
        return false;
    };

    ConversionStats stats;
    std::vector<std::uint8_t> input_buffer(5 * kGroupsPerChunk);
    std::vector<std::uint8_t> pcm_md5_buffer(8 * kGroupsPerChunk);
    while (lds_input) {
        const auto read_started = Clock::now();
        lds_input.read(reinterpret_cast<char*>(input_buffer.data()),
            static_cast<std::streamsize>(input_buffer.size()));
        const auto read_finished = Clock::now();
        const auto got = lds_input.gcount();
        if (got == 0) {
            break;
        }
        if (options.native_stats != nullptr && frame_is_group_aligned) {
            scan_read_ns += elapsed_ns(read_started, read_finished);
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        std::size_t md5_segment_start_group = 0;
        auto decode_segment_started = Clock::now();
        for (std::size_t group = 0; group < groups; ++group) {
            const auto samples = unpack_group_bytes(input_buffer.data() + (group * 5U));
            auto* pcm = pcm_md5_buffer.data() + (group * 8U);
            for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
                write_i16le(pcm + (sample_index * 2U), samples[sample_index]);
            }
            const bool batch_full = append_sample_group(samples);
            if (frame_is_group_aligned && batch_full) {
                const auto decode_segment_finished = Clock::now();
                if (options.native_stats != nullptr) {
                    scan_decode_ns += elapsed_ns(
                        decode_segment_started, decode_segment_finished);
                }
                const auto md5_started = Clock::now();
                pcm_md5.update(
                    pcm_md5_buffer.data() + (md5_segment_start_group * 8U),
                    ((group + 1U) - md5_segment_start_group) * 8U);
                if (options.native_stats != nullptr) {
                    scan_md5_ns += elapsed_ns(md5_started, Clock::now());
                }
                flush_batch();
                md5_segment_start_group = group + 1U;
                decode_segment_started = Clock::now();
            }
        }
        const auto decode_segment_finished = Clock::now();
        if (options.native_stats != nullptr && frame_is_group_aligned) {
            scan_decode_ns += elapsed_ns(decode_segment_started, decode_segment_finished);
        }
        if (md5_segment_start_group < groups) {
            const auto md5_started = Clock::now();
            pcm_md5.update(
                pcm_md5_buffer.data() + (md5_segment_start_group * 8U),
                (groups - md5_segment_start_group) * 8U);
            if (options.native_stats != nullptr && frame_is_group_aligned) {
                scan_md5_ns += elapsed_ns(md5_started, Clock::now());
            }
        }
        stats.input_bytes += static_cast<std::uint64_t>(got);
        stats.samples += groups * 4U;
        if (options.progress_callback) {
            options.progress_callback(stats.input_bytes, stats.samples);
        }
    }

    if (lds_input.bad()) {
        throw std::runtime_error("failed to read LDS input");
    }

    std::vector<std::int32_t> tail_samples;
    const auto complete_sample_count =
        (batch_samples.size() / frame_sample_count) * frame_sample_count;
    if (complete_sample_count < batch_samples.size()) {
        tail_samples.assign(
            batch_samples.begin() + static_cast<std::ptrdiff_t>(complete_sample_count),
            batch_samples.end());
        batch_samples.resize(complete_sample_count);
    }
    flush_batch();
    write_pending_batch();
    if (!tail_samples.empty()) {
        write_native_tail_frame(output, tail_samples, frame_number, options);
        ++frame_number;
    }

    if (options.native_stats != nullptr) {
        if (frame_is_group_aligned) {
            explicit_scan_ns = scan_read_ns + scan_decode_ns + scan_md5_ns;
            options.native_stats->accelerated_scan_ns += explicit_scan_ns;
            options.native_stats->accelerated_scan_read_ns += scan_read_ns;
            options.native_stats->accelerated_scan_decode_ns += scan_decode_ns;
            options.native_stats->accelerated_scan_md5_ns += scan_md5_ns;
        } else {
            const auto ingest_ns = elapsed_ns(ingest_started, Clock::now());
            const auto nested_ns =
                (options.native_stats->accelerated_analyzer_ns - analyzer_before) +
                (options.native_stats->accelerated_selected_write_ns - selected_write_before) +
                (options.native_stats->accelerated_tail_write_ns - tail_write_before);
            if (ingest_ns > nested_ns) {
                options.native_stats->accelerated_scan_ns += ingest_ns - nested_ns;
            }
        }
    }

    const auto streaminfo = make_streaminfo(
        stats, options.frame_samples, options.sample_rate, pcm_md5.digest());
    rewrite_native_flac_streaminfo(output, streaminfo, output_path, options.backend_label);

    output.close();
    if (!output) {
        throw std::runtime_error(
            "failed to finish " + std::string(options.backend_label) + " FLAC output: " +
            output_path);
    }

    std::error_code ec;
    stats.output_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(output_path, ec));
    if (ec) {
        throw std::runtime_error("could not stat output: " + output_path);
    }
    return stats;
}

}  // namespace ldcompress
