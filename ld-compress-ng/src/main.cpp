#include "compressor.h"
#include "flac_codec.h"
#include "hash.h"
#include "lds_codec.h"
#include "metal_backend.h"
#include "metal_devices.h"
#include "opencl_backend.h"
#include "opencl_devices.h"
#include "vulkan_backend.h"
#include "vulkan_devices.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <unistd.h>

#ifndef LDCOMPRESS_VERSION
#define LDCOMPRESS_VERSION "unknown"
#endif

namespace {

constexpr unsigned kDefaultNativeFrameSamples = 4608;
constexpr unsigned kDefaultNativeMaxLpcOrder = 12;
constexpr unsigned kDefaultNativeLpcPrecision = 12;
constexpr unsigned kDefaultNativeMaxRicePartitionOrder = 5;

// A compression/decompression command has at most one active staging output.
// Keep its paths in fixed storage so termination cleanup can use only
// async-signal-safe POSIX calls.
struct SignalSafeStagingOutput {
    volatile sig_atomic_t active = 0;
    char directory[PATH_MAX] {};
    char payload[PATH_MAX] {};
};

SignalSafeStagingOutput g_signal_safe_staging_output;
volatile sig_atomic_t g_termination_cleanup_in_progress = 0;

#if !defined(_WIN32)

void add_termination_signals(sigset_t& signals)
{
    (void)sigaddset(&signals, SIGINT);
    (void)sigaddset(&signals, SIGTERM);
    (void)sigaddset(&signals, SIGHUP);
}

// Blocks termination signals around staging-directory creation so the
// cleanup handler cannot run while staging paths are still registering.
class BlockTerminationSignals final {
public:
    BlockTerminationSignals()
    {
        sigset_t signals {};
        (void)sigemptyset(&signals);
        add_termination_signals(signals);
        if (::sigprocmask(SIG_BLOCK, &signals, &previous_mask_) != 0) {
            throw std::runtime_error("could not block termination signals while staging output");
        }
        blocked_ = true;
    }

    BlockTerminationSignals(const BlockTerminationSignals&) = delete;
    BlockTerminationSignals& operator=(const BlockTerminationSignals&) = delete;

    ~BlockTerminationSignals()
    {
        if (blocked_) {
            (void)::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
        }
    }

private:
    sigset_t previous_mask_ {};
    bool blocked_ = false;
};

#else

// Windows has no sigprocmask, so signal delivery cannot be deferred around
// staging-directory creation. The staging directory name carries six random
// characters and cleanup stays best-effort, so the residual window between
// mkdtemp and path registration is accepted instead.
class BlockTerminationSignals final {
public:
    BlockTerminationSignals() {}
    BlockTerminationSignals(const BlockTerminationSignals&) = delete;
    BlockTerminationSignals& operator=(const BlockTerminationSignals&) = delete;
};

#endif

void register_signal_safe_staging_output(
    const std::filesystem::path& directory,
    const std::filesystem::path& payload)
{
    const auto directory_text = directory.string();
    const auto payload_text = payload.string();
    if (directory_text.size() >= sizeof(g_signal_safe_staging_output.directory) ||
        payload_text.size() >= sizeof(g_signal_safe_staging_output.payload)) {
        throw std::runtime_error("staging output path is too long for termination cleanup");
    }

    BlockTerminationSignals blocked;
    g_signal_safe_staging_output.active = 0;
    std::memcpy(g_signal_safe_staging_output.directory,
        directory_text.c_str(), directory_text.size() + 1U);
    std::memcpy(g_signal_safe_staging_output.payload,
        payload_text.c_str(), payload_text.size() + 1U);
    g_signal_safe_staging_output.active = 1;
}

void unregister_signal_safe_staging_output() noexcept
{
#if !defined(_WIN32)
    sigset_t signals {};
    sigset_t previous_mask {};
    (void)sigemptyset(&signals);
    add_termination_signals(signals);
    if (::sigprocmask(SIG_BLOCK, &signals, &previous_mask) != 0) {
        return;
    }
    g_signal_safe_staging_output.active = 0;
    (void)::sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
#else
    g_signal_safe_staging_output.active = 0;
#endif
}

extern "C" void cleanup_staging_output_on_termination(int signal_number)
{
    if (g_termination_cleanup_in_progress != 0) {
        _exit(128 + signal_number);
    }
    g_termination_cleanup_in_progress = 1;

    if (g_signal_safe_staging_output.active != 0) {
        (void)::unlink(g_signal_safe_staging_output.payload);
        (void)::rmdir(g_signal_safe_staging_output.directory);
    }

#if !defined(_WIN32)
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    (void)sigemptyset(&default_action.sa_mask);
    (void)::sigaction(signal_number, &default_action, nullptr);
    if (::kill(::getpid(), signal_number) != 0) {
        _exit(128 + signal_number);
    }
#else
    // Windows has no sigaction/kill: restore default handling and re-raise.
    (void)std::signal(signal_number, SIG_DFL);
    (void)std::raise(signal_number);
    _exit(128 + signal_number);
#endif
}

void install_staging_output_termination_cleanup()
{
#if !defined(_WIN32)
    struct sigaction action {};
    action.sa_handler = cleanup_staging_output_on_termination;
    (void)sigemptyset(&action.sa_mask);
    add_termination_signals(action.sa_mask);

    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        if (::sigaction(signal_number, &action, nullptr) != 0) {
            throw std::runtime_error("could not install staging-output termination cleanup");
        }
    }
#else
    // Windows: ANSI C signal(). SIGINT is Ctrl+C and SIGBREAK is Ctrl+Break;
    // SIGTERM can be installed but is never raised by the system.
    for (const int signal_number : {SIGINT, SIGTERM, SIGBREAK}) {
        if (std::signal(signal_number, cleanup_staging_output_on_termination) == SIG_ERR) {
            throw std::runtime_error("could not install staging-output termination cleanup");
        }
    }
#endif
}

struct Options {
    bool overwrite = false;
    bool pack = false;
    bool unpack = false;
    bool container_explicit = false;
    bool show_stats = false;
    bool show_progress = false;
    bool bench_include_opencl = false;
    bool bench_include_vulkan = false;
    bool bench_include_metal = false;
    bool bench_reuse_opencl_session = false;
    bool bench_reuse_vulkan_session = false;
    bool bench_reuse_metal_session = false;
    bool level_explicit = false;
    bool native_frame_samples_explicit = false;
    bool native_max_lpc_order_explicit = false;
    bool native_lpc_precision_explicit = false;
    bool native_max_rice_partition_order_explicit = false;
    bool backend_auto = true;
    bool device_index_explicit = false;
    bool opencl_device_index_explicit = false;
    bool vulkan_device_index_explicit = false;
    bool metal_device_index_explicit = false;
    unsigned level = 11;
    unsigned threads = 1;
    unsigned native_frame_samples = kDefaultNativeFrameSamples;
    unsigned native_max_lpc_order = kDefaultNativeMaxLpcOrder;
    unsigned native_lpc_precision = kDefaultNativeLpcPrecision;
    unsigned native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder;
    std::optional<std::size_t> device_index;
    std::optional<std::size_t> opencl_device_index;
    std::optional<std::size_t> vulkan_device_index;
    std::optional<std::size_t> metal_device_index;
    std::vector<unsigned> bench_threads;
    std::vector<unsigned> bench_frame_samples;
    std::vector<unsigned> bench_lpc_orders;
    std::vector<unsigned> bench_lpc_precisions;
    std::vector<unsigned> bench_rice_partition_orders;
    std::vector<ldcompress::NativeAnalysisProfile> bench_analysis_profiles;
    ldcompress::CompressionBackend backend = ldcompress::CompressionBackend::CpuLibFlac;
    ldcompress::FlacContainer container = ldcompress::FlacContainer::Ogg;
    std::string input;
    std::string output;
    std::string source;
};

[[noreturn]] void usage(int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out << "ld-compress-ng compresses LaserDisc RF .lds captures to FLAC-backed .ldf files.\n\n"
        << "Usage:\n"
        << "  ld-compress-ng compress [OPTIONS] INPUT.lds [OUTPUT]\n"
        << "  ld-compress-ng decompress [--overwrite] [--progress] INPUT [OUTPUT]\n"
        << "  ld-compress-ng verify [--source ORIGINAL.lds] INPUT\n"
        << "  ld-compress-ng convert --pack|--unpack [--overwrite] INPUT [OUTPUT]\n"
        << "  ld-compress-ng bench [--threads 8] [--frame-samples N[,N...]] [--lpc-order N[,N...]] [--lpc-precision N[,N...]] [--rice-partition-order N[,N...]] [--analysis-profile NAME[,NAME...]] [--include-opencl] [--include-vulkan] [--include-metal] [--reuse-opencl-session] [--reuse-vulkan-session] [--reuse-metal-session] [--device INDEX|--opencl-device INDEX|--vulkan-device INDEX|--metal-device INDEX] INPUT\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng --version\n"
        << "  ld-compress-ng --help\n\n"
        << "Common examples:\n"
        << "  ld-compress-ng compress capture.lds\n"
        << "  ld-compress-ng decompress capture.ldf\n"
        << "  ld-compress-ng verify --source capture.lds capture.ldf\n"
        << "  ld-compress-ng devices\n"
        << "  ld-compress-ng compress --backend opencl --device INDEX capture.lds\n\n"
        << "Commands:\n"
        << "  compress      Compress packed LDS input. Default backend is auto: Metal, Vulkan,\n"
        << "                hardware OpenCL, then CPU. --level or --container ogg selects CPU.\n"
        << "                Output is INPUT.ldf for every backend.\n"
        << "  decompress    Decode Ogg/native FLAC RF input back to packed LDS output.\n"
        << "  verify        Print compressed and decoded MD5; compare with --source when set.\n"
        << "  convert       Convert between packed LDS and signed 16-bit little-endian PCM.\n"
        << "  bench         Compare backend size/speed using temporary output files.\n"
        << "  devices       List OpenCL, Vulkan, and Metal devices with --device indexes.\n\n"
        << "Compression backends:\n"
        << "  cpu              Default portable Ogg FLAC .ldf backend using libFLAC/libogg.\n"
        << "  opencl           Native FLAC .flac.ldf backend using the selected OpenCL device.\n"
        << "  vulkan           Native FLAC .flac.ldf backend using Vulkan compute.\n"
        << "  metal            Native FLAC .flac.ldf backend using Apple Metal compute.\n"
        << "  native-fixed     Reference/debug scalar native FLAC backend for analysis parity.\n"
        << "  native-verbatim  Reference/debug native FLAC backend using verbatim frames.\n\n"
        << "Compress options:\n"
        << "  --backend auto|cpu|native-verbatim|native-fixed|opencl|vulkan|metal\n"
        << "  --level N                    CPU/libFLAC level, 1..12; default 11.\n"
        << "  --threads N                  Native FLAC frame writer threads; default 1.\n"
        << "  --frame-samples N            Native FLAC block size, 16..4608; default 4608.\n"
        << "  --lpc-order N                Predictive native max LPC order, 0..12; default 12.\n"
        << "  --lpc-precision N            Predictive native LPC precision, 1..15; default 12.\n"
        << "  --rice-partition-order N     Predictive native max Rice partition order, 0..8; default 5.\n"
        << "  --device INDEX               Backend-local OpenCL/Vulkan/Metal device index.\n"
        << "  --opencl-device INDEX        Explicit OpenCL device index.\n"
        << "  --vulkan-device INDEX        Explicit Vulkan device index.\n"
        << "  --metal-device INDEX         Explicit Metal device index.\n"
        << "  --stats                      Print native backend decision stats and timings.\n"
        << "  --container ogg|flac         cpu can write Ogg or native FLAC; native/opencl/vulkan/metal write flac.\n"
        << "  --progress                   Show input progress and elapsed time on stderr.\n"
        << "  --overwrite                  Replace an existing output path.\n\n"
        << "Decompress options:\n"
        << "  --overwrite                  Replace an existing output path.\n"
        << "  --progress                   Show decode progress and elapsed time on stderr.\n\n"
        << "Bench options:\n"
        << "  --analysis-profile NAME      exact, order-guess-exact-rice,\n"
        << "                               order-guess-mean-rice,\n"
        << "                               order-guess-mean-estimate-rice,\n"
        << "                               subdivide-tukey3-mean-rice, or\n"
        << "                               subdivide-tukey3-mean-estimate-rice.\n"
        << "  --reuse-opencl-session       Reuse OpenCL setup across benchmark rows.\n"
        << "  --reuse-vulkan-session       Reuse Vulkan setup across benchmark rows.\n\n"
        << "  --reuse-metal-session        Reuse Metal setup across benchmark rows.\n\n"
        << "More details: README.md and docs/build-and-testing.md\n";
    std::exit(exit_code);
}

int version()
{
    std::cout << "ld-compress-ng " << LDCOMPRESS_VERSION << '\n';
    return 0;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

bool is_native_flac_backend(ldcompress::CompressionBackend backend)
{
    return backend == ldcompress::CompressionBackend::NativeVerbatimFlac ||
        backend == ldcompress::CompressionBackend::NativeFixedFlac ||
        backend == ldcompress::CompressionBackend::OpenClNativeFlac ||
        backend == ldcompress::CompressionBackend::VulkanNativeFlac ||
        backend == ldcompress::CompressionBackend::MetalNativeFlac;
}

bool is_accelerated_native_backend(ldcompress::CompressionBackend backend)
{
    return backend == ldcompress::CompressionBackend::OpenClNativeFlac ||
        backend == ldcompress::CompressionBackend::VulkanNativeFlac ||
        backend == ldcompress::CompressionBackend::MetalNativeFlac;
}

std::optional<std::size_t> effective_opencl_device_index(const Options& options)
{
    return options.opencl_device_index.has_value()
        ? options.opencl_device_index
        : options.device_index;
}

std::optional<std::size_t> effective_vulkan_device_index(const Options& options)
{
    return options.vulkan_device_index.has_value()
        ? options.vulkan_device_index
        : options.device_index;
}

std::optional<std::size_t> effective_metal_device_index(const Options& options)
{
    return options.metal_device_index.has_value()
        ? options.metal_device_index
        : options.device_index;
}

std::optional<std::size_t> available_opencl_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::opencl_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_opencl_devices()) {
        if (device.available &&
            (!requested_index.has_value() || device.flat_index == *requested_index)) {
            return device.flat_index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> available_opencl_accelerator_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::opencl_support_built()) {
        return std::nullopt;
    }

    for (const auto& device : ldcompress::list_opencl_devices()) {
        if (ldcompress::opencl_device_is_auto_eligible(device) &&
            (!requested_index.has_value() || device.flat_index == *requested_index)) {
            return device.flat_index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> available_vulkan_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::vulkan_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_vulkan_devices();
    if (requested_index.has_value()) {
        if (*requested_index < devices.size()) {
            const auto& device = devices[*requested_index];
            if (device.available && device.shader_int64) {
                return device.index;
            }
        }
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && device.shader_int64 && device.device_type == "discrete-gpu") {
            return device.index;
        }
    }
    for (const auto& device : devices) {
        if (device.available && device.shader_int64 && device.device_type != "cpu") {
            return device.index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> available_metal_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::metal_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_metal_devices();
    if (requested_index.has_value()) {
        if (*requested_index < devices.size() && devices[*requested_index].available) {
            return devices[*requested_index].index;
        }
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && !device.low_power) {
            return device.index;
        }
    }
    for (const auto& device : devices) {
        if (device.available) {
            return device.index;
        }
    }

    return std::nullopt;
}

ldcompress::CompressionBackend select_automatic_backend(const Options& options)
{
    if (available_metal_device_index(effective_metal_device_index(options)).has_value()) {
        return ldcompress::CompressionBackend::MetalNativeFlac;
    }
    if (available_vulkan_device_index(effective_vulkan_device_index(options)).has_value()) {
        return ldcompress::CompressionBackend::VulkanNativeFlac;
    }
    if (available_opencl_accelerator_device_index(
            effective_opencl_device_index(options)).has_value()) {
        return ldcompress::CompressionBackend::OpenClNativeFlac;
    }
    return ldcompress::CompressionBackend::CpuLibFlac;
}

bool automatic_backend_is_constrained_to_cpu(const Options& options)
{
    return options.level_explicit ||
        (options.container_explicit && options.container == ldcompress::FlacContainer::Ogg);
}

ldcompress::FlacContainer default_container_for_backend(ldcompress::CompressionBackend backend)
{
    return is_native_flac_backend(backend)
        ? ldcompress::FlacContainer::Native
        : ldcompress::FlacContainer::Ogg;
}

std::string default_convert_output(const Options& options)
{
    const std::filesystem::path input_path(options.input);
    if (options.unpack) {
        return input_path.stem().string() + ".s16";
    }
    return input_path.stem().string() + ".lds";
}

std::string default_compress_output(const Options& options)
{
    const std::filesystem::path input_path(options.input);
    return input_path.stem().string() + ".ldf";
}

std::string default_decompress_output(const std::string& input)
{
    const std::filesystem::path input_path(input);
    const auto filename = input_path.filename().string();
    if (ends_with(filename, ".raw.oga")) {
        return filename.substr(0, filename.size() - std::string_view(".raw.oga").size()) + ".lds";
    }
    if (ends_with(filename, ".flac.ldf")) {
        return filename.substr(0, filename.size() - std::string_view(".flac.ldf").size()) + ".lds";
    }
    if (ends_with(filename, ".ldf")) {
        return filename.substr(0, filename.size() - std::string_view(".ldf").size()) + ".lds";
    }
    return input_path.stem().string() + ".lds";
}

std::optional<std::uint64_t> regular_input_size_or_unknown(const std::string& input)
{
    std::error_code ec;
    const auto status = std::filesystem::status(input, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return std::nullopt;
    }

    const auto size = std::filesystem::file_size(input, ec);
    if (ec || size > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

void ensure_output_allowed(const std::string& path, bool overwrite)
{
    if (!overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error("output already exists: " + path + " (use --overwrite)");
    }
}

std::filesystem::path normalized_absolute(const std::string& path)
{
    return std::filesystem::absolute(std::filesystem::path(path)).lexically_normal();
}

bool same_or_equivalent_path(const std::string& lhs, const std::string& rhs)
{
    std::error_code ec;
    if (std::filesystem::exists(lhs, ec) && std::filesystem::exists(rhs, ec)) {
        if (std::filesystem::equivalent(lhs, rhs, ec)) {
            return true;
        }
        ec.clear();
    }

    return normalized_absolute(lhs) == normalized_absolute(rhs);
}

void ensure_distinct_input_output(const std::string& input, const std::string& output)
{
    if (same_or_equivalent_path(input, output)) {
        throw std::runtime_error("input and output refer to the same file: " + input);
    }
}

class TemporaryOutput final {
public:
    explicit TemporaryOutput(const std::string& output)
    {
        const std::filesystem::path output_path(output);
        const auto parent = output_path.parent_path().empty()
            ? std::filesystem::current_path()
            : output_path.parent_path();
        std::string template_path =
            (parent / ("." + output_path.filename().string() + ".tmp-XXXXXX")).string();
        // Keep termination signals pending from the moment the directory can
        // become visible until its cleanup paths have been registered.
        BlockTerminationSignals blocked;
        if (::mkdtemp(template_path.data()) == nullptr) {
            throw std::runtime_error("could not create private staging directory for: " + output);
        }
        directory_ = std::move(template_path);
        payload_ = directory_ / "payload";
        try {
            register_signal_safe_staging_output(directory_, payload_);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove_all(directory_, ec);
            throw;
        }
    }

    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;

    ~TemporaryOutput()
    {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
        if (!ec) {
            unregister_signal_safe_staging_output();
        }
    }

    const std::filesystem::path& payload() const
    {
        return payload_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path payload_;
};

int rename_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
#if defined(__APPLE__)
    return ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned kRenameNoReplace = 1U;
    return static_cast<int>(::syscall(SYS_renameat2,
        AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), kRenameNoReplace));
#elif defined(_WIN32)
    // MoveFileExW without MOVEFILE_REPLACE_EXISTING is an atomic no-replace
    // rename on Windows: it fails with ERROR_ALREADY_EXISTS when the
    // destination is present, so publication never clobbers a concurrent
    // writer.
    if (::MoveFileExW(source.c_str(), destination.c_str(), 0U) != 0) {
        return 0;
    }
    const ::DWORD error = ::GetLastError();
    errno = (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        ? EEXIST
        : EINVAL;
    return -1;
#else
    (void)source;
    (void)destination;
    errno = ENOSYS;
    return -1;
#endif
}

bool rename_no_replace_is_unavailable(int error)
{
    return error == ENOSYS || error == EINVAL || error == ENOTSUP;
}

void publish_temporary_output(
    const TemporaryOutput& staging_output,
    const std::string& output,
    bool overwrite)
{
    if (overwrite) {
        std::filesystem::rename(staging_output.payload(), output);
        return;
    }

    if (rename_no_replace(staging_output.payload(), output) == 0) {
        return;
    }

    const int rename_error = errno;
    if (rename_error == EEXIST) {
        throw std::runtime_error("output already exists: " + output + " (use --overwrite)");
    }
    if (!rename_no_replace_is_unavailable(rename_error)) {
        throw std::runtime_error("could not publish output: " + output + ": " +
            std::string(std::strerror(rename_error)));
    }

#if !defined(_WIN32)
    // Older Linux kernels and other POSIX targets may not provide an atomic
    // no-replace rename. Preserve the original hard-link publication path for
    // filesystems where it is available.
    if (::link(staging_output.payload().c_str(), output.c_str()) == 0) {
        return;
    }
#else
    // Windows reached here without ERROR_ALREADY_EXISTS and without a
    // no-replace rename failure, which MoveFileExW reports directly; treat
    // the publication as unsupported rather than clobbering anything.
    errno = ENOSYS;
#endif

    const int error = errno;
    if (error == EEXIST) {
        throw std::runtime_error("output already exists: " + output + " (use --overwrite)");
    }
    throw std::runtime_error("could not publish output: " + output + ": " +
        std::string(std::strerror(error)));
}

unsigned parse_bounded_unsigned(
    std::string_view text,
    std::string_view name,
    unsigned min_value,
    unsigned max_value)
{
    unsigned value = 0;
    if (text.empty()) {
        throw std::runtime_error("empty " + std::string(name));
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid " + std::string(name) + ": " + std::string(text));
        }
        const auto digit = static_cast<unsigned>(ch - '0');
        if (digit > max_value || value > (max_value - digit) / 10U) {
            throw std::runtime_error(std::string(name) + " must be " +
                std::to_string(min_value) + ".." + std::to_string(max_value));
        }
        value = (value * 10U) + digit;
    }
    if (value < min_value || value > max_value) {
        throw std::runtime_error(std::string(name) + " must be " +
            std::to_string(min_value) + ".." + std::to_string(max_value));
    }
    return value;
}

unsigned parse_level(std::string_view text)
{
    return parse_bounded_unsigned(text, "compression level", 1, 12);
}

unsigned parse_threads(std::string_view text)
{
    return parse_bounded_unsigned(text, "thread count", 1, 1024);
}

std::size_t parse_device_index(std::string_view text)
{
    std::size_t value = 0;
    if (text.empty()) {
        throw std::runtime_error("empty device index");
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid device index: " + std::string(text));
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            throw std::runtime_error("device index is too large: " + std::string(text));
        }
        value = (value * 10U) + digit;
    }
    return value;
}

std::vector<unsigned> parse_thread_list(std::string_view text)
{
    std::vector<unsigned> threads;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string_view item = comma == std::string_view::npos
            ? text.substr(offset)
            : text.substr(offset, comma - offset);
        threads.push_back(parse_threads(item));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return threads;
}

std::vector<unsigned> parse_bounded_unsigned_list(
    std::string_view text,
    std::string_view name,
    unsigned min_value,
    unsigned max_value)
{
    std::vector<unsigned> values;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string_view item = comma == std::string_view::npos
            ? text.substr(offset)
            : text.substr(offset, comma - offset);
        values.push_back(parse_bounded_unsigned(item, name, min_value, max_value));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return values;
}

ldcompress::NativeAnalysisProfile parse_native_analysis_profile(std::string_view text)
{
    if (text == "exact") {
        return ldcompress::NativeAnalysisProfile::Exact;
    }
    if (text == "order-guess-exact-rice") {
        return ldcompress::NativeAnalysisProfile::OrderGuessExactRice;
    }
    if (text == "order-guess-mean-rice") {
        return ldcompress::NativeAnalysisProfile::OrderGuessMeanRice;
    }
    if (text == "order-guess-mean-estimate-rice") {
        return ldcompress::NativeAnalysisProfile::OrderGuessMeanEstimateRice;
    }
    if (text == "subdivide-tukey3-mean-rice") {
        return ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanRice;
    }
    if (text == "subdivide-tukey3-mean-estimate-rice") {
        return ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
    }
    throw std::runtime_error("unknown analysis profile: " + std::string(text));
}

std::vector<ldcompress::NativeAnalysisProfile> parse_native_analysis_profile_list(
    std::string_view text)
{
    std::vector<ldcompress::NativeAnalysisProfile> profiles;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string_view item = comma == std::string_view::npos
            ? text.substr(offset)
            : text.substr(offset, comma - offset);
        profiles.push_back(parse_native_analysis_profile(item));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
    return profiles;
}

Options parse_compress(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--stats") {
            options.show_stats = true;
        } else if (arg == "--progress") {
            options.show_progress = true;
        } else if (arg == "--backend") {
            if (++i >= argc) {
                throw std::runtime_error("--backend requires a value");
            }
            const std::string_view backend(argv[i]);
            if (backend == "auto") {
                options.backend_auto = true;
            } else if (backend == "cpu" || backend == "libflac") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::CpuLibFlac;
            } else if (backend == "native-verbatim" || backend == "verbatim") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::NativeVerbatimFlac;
            } else if (backend == "native-fixed" || backend == "fixed-rice") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::NativeFixedFlac;
            } else if (backend == "opencl" || backend == "gpu") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::OpenClNativeFlac;
            } else if (backend == "vulkan") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::VulkanNativeFlac;
            } else if (backend == "metal") {
                options.backend_auto = false;
                options.backend = ldcompress::CompressionBackend::MetalNativeFlac;
            } else {
                throw std::runtime_error("unknown backend: " + std::string(backend));
            }
        } else if (arg == "--level") {
            if (++i >= argc) {
                throw std::runtime_error("--level requires a value");
            }
            options.level_explicit = true;
            options.level = parse_level(argv[i]);
        } else if (arg == "--threads") {
            if (++i >= argc) {
                throw std::runtime_error("--threads requires a value");
            }
            options.threads = parse_threads(argv[i]);
        } else if (arg == "--frame-samples") {
            if (++i >= argc) {
                throw std::runtime_error("--frame-samples requires a value");
            }
            options.native_frame_samples_explicit = true;
            options.native_frame_samples = parse_bounded_unsigned(
                argv[i], "native FLAC frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-order requires a value");
            }
            options.native_max_lpc_order_explicit = true;
            options.native_max_lpc_order = parse_bounded_unsigned(
                argv[i], "native FLAC max LPC order", 0, 12);
        } else if (arg == "--lpc-precision") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-precision requires a value");
            }
            options.native_lpc_precision_explicit = true;
            options.native_lpc_precision = parse_bounded_unsigned(
                argv[i], "native FLAC LPC coefficient precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            if (++i >= argc) {
                throw std::runtime_error("--rice-partition-order requires a value");
            }
            options.native_max_rice_partition_order_explicit = true;
            options.native_max_rice_partition_order = parse_bounded_unsigned(
                argv[i], "native FLAC max Rice partition order", 0, 8);
        } else if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.device_index_explicit = true;
            options.device_index = parse_device_index(argv[i]);
        } else if (arg == "--opencl-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.opencl_device_index_explicit = true;
            options.opencl_device_index = parse_device_index(argv[i]);
        } else if (arg == "--vulkan-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.vulkan_device_index_explicit = true;
            options.vulkan_device_index = parse_device_index(argv[i]);
        } else if (arg == "--metal-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.metal_device_index_explicit = true;
            options.metal_device_index = parse_device_index(argv[i]);
        } else if (arg == "--container") {
            if (++i >= argc) {
                throw std::runtime_error("--container requires a value");
            }
            const std::string_view container(argv[i]);
            options.container_explicit = true;
            if (container == "ogg") {
                options.container = ldcompress::FlacContainer::Ogg;
            } else if (container == "flac" || container == "native") {
                options.container = ldcompress::FlacContainer::Native;
            } else {
                throw std::runtime_error("unknown container: " + std::string(container));
            }
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("compress expects INPUT and optional OUTPUT");
    }

    if (options.backend_auto) {
        options.backend = automatic_backend_is_constrained_to_cpu(options)
            ? ldcompress::CompressionBackend::CpuLibFlac
            : select_automatic_backend(options);
    }

    if (!options.container_explicit) {
        options.container = default_container_for_backend(options.backend);
    }

    const bool native_predictive_options_explicit =
        options.native_max_lpc_order_explicit ||
        options.native_lpc_precision_explicit ||
        options.native_max_rice_partition_order_explicit;
    const bool native_tuning_options_explicit =
        options.native_frame_samples_explicit || native_predictive_options_explicit;

    if (options.level_explicit &&
        options.backend != ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--level is currently supported only by the cpu backend");
    }
    if (options.threads != 1 &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--threads is currently supported only by native FLAC backends");
    }
    if (options.show_stats &&
        options.backend == ldcompress::CompressionBackend::CpuLibFlac) {
        throw std::runtime_error("--stats is currently supported only by native FLAC backends");
    }
    if (options.device_index_explicit && !is_accelerated_native_backend(options.backend)) {
        throw std::runtime_error("--device is currently supported only by opencl, vulkan, and metal backends");
    }
    if (options.backend == ldcompress::CompressionBackend::OpenClNativeFlac &&
        (options.vulkan_device_index_explicit || options.metal_device_index_explicit)) {
        throw std::runtime_error("--vulkan-device and --metal-device cannot be used with the opencl backend");
    }
    if (options.backend == ldcompress::CompressionBackend::VulkanNativeFlac &&
        (options.opencl_device_index_explicit || options.metal_device_index_explicit)) {
        throw std::runtime_error("--opencl-device and --metal-device cannot be used with the vulkan backend");
    }
    if (options.backend == ldcompress::CompressionBackend::MetalNativeFlac &&
        (options.opencl_device_index_explicit || options.vulkan_device_index_explicit)) {
        throw std::runtime_error("--opencl-device and --vulkan-device cannot be used with the metal backend");
    }
    if (options.opencl_device_index_explicit &&
        options.backend != ldcompress::CompressionBackend::OpenClNativeFlac) {
        throw std::runtime_error("--opencl-device is currently supported only by the opencl backend");
    }
    if (options.vulkan_device_index_explicit &&
        options.backend != ldcompress::CompressionBackend::VulkanNativeFlac) {
        throw std::runtime_error("--vulkan-device is currently supported only by the vulkan backend");
    }
    if (options.metal_device_index_explicit &&
        options.backend != ldcompress::CompressionBackend::MetalNativeFlac) {
        throw std::runtime_error("--metal-device is currently supported only by the metal backend");
    }
    if (options.backend == ldcompress::CompressionBackend::CpuLibFlac &&
        native_tuning_options_explicit) {
        throw std::runtime_error("--frame-samples, --lpc-order, --lpc-precision, and --rice-partition-order are supported only by native FLAC backends");
    }
    if (options.backend == ldcompress::CompressionBackend::NativeVerbatimFlac &&
        native_predictive_options_explicit) {
        throw std::runtime_error("--lpc-order, --lpc-precision, and --rice-partition-order are supported only by predictive native FLAC backends");
    }

    if (is_native_flac_backend(options.backend) &&
        options.container != ldcompress::FlacContainer::Native) {
        throw std::runtime_error(std::string(ldcompress::backend_name(options.backend)) +
            " backend writes native FLAC only");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_compress_output(options);
    return options;
}

Options parse_decompress(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--progress") {
            options.show_progress = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("decompress expects INPUT and optional OUTPUT");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_decompress_output(options.input);
    return options;
}

Options parse_verify(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--source") {
            if (++i >= argc) {
                throw std::runtime_error("--source requires a value");
            }
            options.source = argv[i];
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.size() != 1) {
        throw std::runtime_error("verify expects exactly one INPUT");
    }

    options.input = positional[0];
    return options;
}

Options parse_convert(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--pack") {
            options.pack = true;
        } else if (arg == "--unpack") {
            options.unpack = true;
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (options.pack == options.unpack) {
        throw std::runtime_error("specify exactly one of --pack or --unpack");
    }
    if (positional.empty() || positional.size() > 2) {
        throw std::runtime_error("convert expects INPUT and optional OUTPUT");
    }

    options.input = positional[0];
    options.output = positional.size() == 2 ? positional[1] : default_convert_output(options);
    return options;
}

Options parse_bench(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--threads") {
            if (++i >= argc) {
                throw std::runtime_error("--threads requires a value");
            }
            options.bench_threads = parse_thread_list(argv[i]);
        } else if (arg == "--frame-samples") {
            if (++i >= argc) {
                throw std::runtime_error("--frame-samples requires a value");
            }
            options.bench_frame_samples = parse_bounded_unsigned_list(
                argv[i], "native FLAC frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-order requires a value");
            }
            options.bench_lpc_orders = parse_bounded_unsigned_list(
                argv[i], "native FLAC max LPC order", 0, 12);
        } else if (arg == "--lpc-precision") {
            if (++i >= argc) {
                throw std::runtime_error("--lpc-precision requires a value");
            }
            options.bench_lpc_precisions = parse_bounded_unsigned_list(
                argv[i], "native FLAC LPC coefficient precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            if (++i >= argc) {
                throw std::runtime_error("--rice-partition-order requires a value");
            }
            options.bench_rice_partition_orders = parse_bounded_unsigned_list(
                argv[i], "native FLAC max Rice partition order", 0, 8);
        } else if (arg == "--analysis-profile") {
            if (++i >= argc) {
                throw std::runtime_error("--analysis-profile requires a value");
            }
            options.bench_analysis_profiles =
                parse_native_analysis_profile_list(argv[i]);
        } else if (arg == "--include-opencl") {
            options.bench_include_opencl = true;
        } else if (arg == "--include-vulkan") {
            options.bench_include_vulkan = true;
        } else if (arg == "--include-metal") {
            options.bench_include_metal = true;
        } else if (arg == "--reuse-opencl-session") {
            options.bench_reuse_opencl_session = true;
        } else if (arg == "--reuse-vulkan-session") {
            options.bench_reuse_vulkan_session = true;
        } else if (arg == "--reuse-metal-session") {
            options.bench_reuse_metal_session = true;
        } else if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.device_index_explicit = true;
            options.device_index = parse_device_index(argv[i]);
        } else if (arg == "--opencl-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.opencl_device_index_explicit = true;
            options.opencl_device_index = parse_device_index(argv[i]);
        } else if (arg == "--vulkan-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.vulkan_device_index_explicit = true;
            options.vulkan_device_index = parse_device_index(argv[i]);
        } else if (arg == "--metal-device") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            options.metal_device_index_explicit = true;
            options.metal_device_index = parse_device_index(argv[i]);
        } else if (arg == "--help" || arg == "-h") {
            usage(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else {
            positional.emplace_back(arg);
        }
    }

    if (positional.size() != 1) {
        throw std::runtime_error("bench expects exactly one INPUT");
    }
    if (options.bench_threads.empty()) {
        options.bench_threads.push_back(1);
    }
    if (options.bench_frame_samples.empty()) {
        options.bench_frame_samples.push_back(options.native_frame_samples);
    }
    if (options.bench_lpc_orders.empty()) {
        options.bench_lpc_orders.push_back(options.native_max_lpc_order);
    }
    if (options.bench_lpc_precisions.empty()) {
        options.bench_lpc_precisions.push_back(options.native_lpc_precision);
    }
    if (options.bench_rice_partition_orders.empty()) {
        options.bench_rice_partition_orders.push_back(options.native_max_rice_partition_order);
    }
    if (options.bench_analysis_profiles.empty()) {
        options.bench_analysis_profiles.push_back(ldcompress::NativeAnalysisProfile::Exact);
    }
    if (options.device_index_explicit &&
        !options.bench_include_opencl &&
        !options.bench_include_vulkan &&
        !options.bench_include_metal) {
        throw std::runtime_error("--device is supported by bench only with --include-opencl, --include-vulkan, or --include-metal");
    }
    const auto included_accelerator_count =
        (options.bench_include_opencl ? 1 : 0) +
        (options.bench_include_vulkan ? 1 : 0) +
        (options.bench_include_metal ? 1 : 0);
    if (options.device_index_explicit && included_accelerator_count > 1) {
        throw std::runtime_error("--device is ambiguous when bench includes multiple accelerators; use --opencl-device, --vulkan-device, and --metal-device");
    }
    if (options.opencl_device_index_explicit && !options.bench_include_opencl) {
        throw std::runtime_error("--opencl-device is supported by bench only with --include-opencl");
    }
    if (options.vulkan_device_index_explicit && !options.bench_include_vulkan) {
        throw std::runtime_error("--vulkan-device is supported by bench only with --include-vulkan");
    }
    if (options.metal_device_index_explicit && !options.bench_include_metal) {
        throw std::runtime_error("--metal-device is supported by bench only with --include-metal");
    }
    if (options.bench_reuse_opencl_session && !options.bench_include_opencl) {
        throw std::runtime_error("--reuse-opencl-session requires --include-opencl");
    }
    if (options.bench_reuse_vulkan_session && !options.bench_include_vulkan) {
        throw std::runtime_error("--reuse-vulkan-session requires --include-vulkan");
    }
    if (options.bench_reuse_metal_session && !options.bench_include_metal) {
        throw std::runtime_error("--reuse-metal-session requires --include-metal");
    }

    options.input = positional[0];
    return options;
}

class HashingStreambuf final : public std::streambuf {
public:
    std::uint64_t bytes() const { return bytes_; }
    const ldcompress::Md5& md5() const { return md5_; }

protected:
    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        md5_.update(s, static_cast<std::uint64_t>(count));
        bytes_ += static_cast<std::uint64_t>(count);
        return count;
    }

    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }
        const auto byte = static_cast<unsigned char>(ch);
        md5_.update(&byte, 1);
        ++bytes_;
        return ch;
    }

private:
    ldcompress::Md5 md5_;
    std::uint64_t bytes_ = 0;
};

class DecompressionProgressReporter final {
public:
    explicit DecompressionProgressReporter(bool enabled)
        : enabled_(enabled)
        , started_(std::chrono::steady_clock::now())
    {
    }

    ~DecompressionProgressReporter()
    {
        finish();
    }

    void report(std::uint64_t decoded_samples, std::uint64_t total_samples)
    {
        if (!enabled_ || finished_) {
            return;
        }

        decoded_samples_ = decoded_samples;
        total_samples_ = total_samples;
        have_progress_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (!rendered_ || now - last_render_ >= kUpdateInterval) {
            render(now);
        }
    }

    void finish() noexcept
    {
        if (!enabled_ || finished_ || !have_progress_) {
            return;
        }

        try {
            render(std::chrono::steady_clock::now());
            std::cerr << '\n' << std::flush;
        } catch (...) {
            // Progress reporting must never obscure the decode result.
        }
        finished_ = true;
    }

private:
    static constexpr auto kUpdateInterval = std::chrono::milliseconds(250);

    void render(std::chrono::steady_clock::time_point now)
    {
        std::cerr << "\rdecompressing: ";
        if (total_samples_ != 0) {
            if (decoded_samples_ > total_samples_) {
                std::cerr << ">=100% (" << decoded_samples_ << '/'
                          << total_samples_ << " samples; STREAMINFO total exceeded)";
            } else {
                const auto percent = static_cast<unsigned>(
                    (decoded_samples_ * 100U) / total_samples_);
                std::cerr << percent << "% (" << decoded_samples_ << '/'
                          << total_samples_ << " samples)";
            }
        } else {
            std::cerr << decoded_samples_ << " samples";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_);
        std::cerr << " decoded, " << elapsed.count() << "s" << std::flush;
        last_render_ = now;
        rendered_ = true;
    }

    bool enabled_ = false;
    bool have_progress_ = false;
    bool rendered_ = false;
    bool finished_ = false;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_render_ {};
    std::uint64_t decoded_samples_ = 0;
    std::uint64_t total_samples_ = 0;
};

class CompressionProgressReporter final {
public:
    CompressionProgressReporter(bool enabled, std::optional<std::uint64_t> total_input_bytes)
        : enabled_(enabled)
        , total_input_bytes_(total_input_bytes)
        , started_(std::chrono::steady_clock::now())
    {
    }

    ~CompressionProgressReporter()
    {
        finish(false);
    }

    void report(std::uint64_t consumed_input_bytes, std::uint64_t consumed_samples)
    {
        if (!enabled_ || finished_) {
            return;
        }

        consumed_input_bytes_ = consumed_input_bytes;
        consumed_samples_ = consumed_samples;
        have_progress_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (!rendered_ || now - last_render_ >= kUpdateInterval || input_is_complete()) {
            render(now, false);
        }
    }

    void finish() noexcept
    {
        finish(true);
    }

private:
    static constexpr auto kUpdateInterval = std::chrono::milliseconds(250);

    void finish(bool completed) noexcept
    {
        if (!enabled_ || finished_ || !have_progress_) {
            return;
        }

        try {
            render(std::chrono::steady_clock::now(), completed);
            std::cerr << '\n' << std::flush;
        } catch (...) {
            // Progress reporting must never obscure the compression result.
        }
        finished_ = true;
    }

    static std::string format_binary_size(std::uint64_t value)
    {
        constexpr std::array<std::string_view, 5> units {"B", "KiB", "MiB", "GiB", "TiB"};
        double scaled = static_cast<double>(value);
        std::size_t unit = 0;
        while (scaled >= 1024.0 && unit + 1U < units.size()) {
            scaled /= 1024.0;
            ++unit;
        }

        std::ostringstream output;
        if (unit == 0) {
            output << value;
        } else if (scaled < 10.0) {
            output << std::fixed << std::setprecision(1) << scaled;
        } else {
            output << std::fixed << std::setprecision(0) << scaled;
        }
        output << ' ' << units[unit];
        return output.str();
    }

    static std::string format_sample_count(std::uint64_t value)
    {
        constexpr std::array<std::string_view, 5> units {
            "samples", "ksamples", "Msamples", "Gsamples", "Tsamples"};
        double scaled = static_cast<double>(value);
        std::size_t unit = 0;
        while (scaled >= 1000.0 && unit + 1U < units.size()) {
            scaled /= 1000.0;
            ++unit;
        }

        std::ostringstream output;
        if (unit == 0) {
            output << value;
        } else if (scaled < 10.0) {
            output << std::fixed << std::setprecision(1) << scaled;
        } else {
            output << std::fixed << std::setprecision(0) << scaled;
        }
        output << ' ' << units[unit];
        return output.str();
    }

    static std::string format_eta(std::uint64_t seconds)
    {
        std::ostringstream output;
        if (seconds < 60U) {
            output << seconds << 's';
            return output.str();
        }

        const auto minutes = seconds / 60U;
        const auto remaining_seconds = seconds % 60U;
        if (minutes < 60U) {
            output << minutes << 'm' << ' ' << remaining_seconds << 's';
            return output.str();
        }

        const auto hours = minutes / 60U;
        const auto remaining_minutes = minutes % 60U;
        if (hours < 24U) {
            output << hours << 'h' << ' ' << remaining_minutes << 'm';
            return output.str();
        }

        output << (hours / 24U) << 'd' << ' ' << (hours % 24U) << 'h';
        return output.str();
    }

    void update_rate(std::chrono::steady_clock::time_point now)
    {
        if (!have_rendered_progress_ || consumed_input_bytes_ <= rendered_input_bytes_) {
            return;
        }

        const auto elapsed = std::chrono::duration<double>(now - last_render_).count();
        if (elapsed <= 0.0) {
            return;
        }
        bytes_per_second_ = static_cast<std::uint64_t>(
            static_cast<double>(consumed_input_bytes_ - rendered_input_bytes_) / elapsed);
    }

    bool input_is_complete() const
    {
        return total_input_bytes_.has_value() && *total_input_bytes_ != 0 &&
            consumed_input_bytes_ >= *total_input_bytes_;
    }

    std::optional<std::uint64_t> average_rate(
        std::chrono::steady_clock::time_point now) const
    {
        if (consumed_input_bytes_ == 0) {
            return std::nullopt;
        }
        const auto elapsed = std::chrono::duration<double>(now - started_).count();
        if (elapsed <= 0.0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(
            static_cast<double>(consumed_input_bytes_) / elapsed);
    }

    void render(std::chrono::steady_clock::time_point now, bool completed)
    {
        const bool input_complete = input_is_complete();
        if (!completed && !input_complete) {
            update_rate(now);
        }
        std::cerr << "\rcompressing: ";
        if (total_input_bytes_.has_value() && *total_input_bytes_ != 0) {
            if (consumed_input_bytes_ > *total_input_bytes_) {
                std::cerr << ">=100% (";
            } else {
                const auto percent = static_cast<unsigned>(
                    ((consumed_input_bytes_ / *total_input_bytes_) * 100U) +
                    (((consumed_input_bytes_ % *total_input_bytes_) * 100U) /
                        *total_input_bytes_));
                std::cerr << percent << "% (";
            }
            std::cerr << format_binary_size(consumed_input_bytes_) << '/'
                      << format_binary_size(*total_input_bytes_) << ", "
                      << format_sample_count(consumed_samples_) << ')';
        } else {
            std::cerr << format_binary_size(consumed_input_bytes_) << ", "
                      << format_sample_count(consumed_samples_);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_);
        if (completed) {
            std::cerr << ", complete";
            if (const auto rate = average_rate(now); rate.has_value()) {
                std::cerr << ", average " << format_binary_size(*rate) << "/s";
            }
            std::cerr << ", elapsed " << elapsed.count() << "s" << std::flush;
        } else if (input_complete) {
            std::cerr << ", input complete; finalizing, elapsed " << elapsed.count() << "s"
                      << std::flush;
        } else {
            if (bytes_per_second_.has_value()) {
                std::cerr << ", input " << format_binary_size(*bytes_per_second_) << "/s";
            }
            std::cerr << ", elapsed " << elapsed.count() << "s" << std::flush;
            if (total_input_bytes_.has_value() && *total_input_bytes_ > consumed_input_bytes_ &&
                bytes_per_second_.has_value() && *bytes_per_second_ != 0) {
                const auto remaining_bytes = *total_input_bytes_ - consumed_input_bytes_;
                const auto eta_seconds = (remaining_bytes / *bytes_per_second_) +
                    ((remaining_bytes % *bytes_per_second_) == 0 ? 0U : 1U);
                std::cerr << ", ETA " << format_eta(eta_seconds) << std::flush;
            }
        }
        if (::isatty(STDERR_FILENO) != 0) {
            std::cerr << "\x1b[K" << std::flush;
        }
        last_render_ = now;
        rendered_input_bytes_ = consumed_input_bytes_;
        have_rendered_progress_ = true;
        rendered_ = true;
    }

    bool enabled_ = false;
    bool have_progress_ = false;
    bool rendered_ = false;
    bool finished_ = false;
    std::optional<std::uint64_t> total_input_bytes_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_render_ {};
    std::uint64_t consumed_input_bytes_ = 0;
    std::uint64_t consumed_samples_ = 0;
    std::uint64_t rendered_input_bytes_ = 0;
    std::optional<std::uint64_t> bytes_per_second_;
    bool have_rendered_progress_ = false;
};

template <std::size_t N>
void print_nonzero_counts(
    std::ostream& output,
    std::string_view label,
    const std::array<std::uint64_t, N>& counts)
{
    output << label << ':';
    bool any = false;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) {
            continue;
        }
        output << (any ? ", " : " ") << i << '=' << counts[i];
        any = true;
    }
    if (!any) {
        output << " none";
    }
    output << '\n';
}

template <std::size_t N>
std::string summarize_nonzero_counts(
    const std::array<std::uint64_t, N>& counts,
    std::size_t max_items = 3)
{
    std::vector<std::pair<std::uint64_t, std::size_t>> nonzero;
    nonzero.reserve(counts.size());
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != 0) {
            nonzero.emplace_back(counts[i], i);
        }
    }
    if (nonzero.empty()) {
        return "-";
    }

    std::sort(nonzero.begin(), nonzero.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });

    std::ostringstream out;
    const auto limit = std::min(max_items, nonzero.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            out << ',';
        }
        out << nonzero[i].second << ':' << nonzero[i].first;
    }
    return out.str();
}

std::string summarize_subframes(const ldcompress::NativeCompressionStats& stats)
{
    if (stats.frames == 0) {
        return "-";
    }
    std::ostringstream out;
    out << "c" << stats.constant_frames
        << ",f" << stats.fixed_rice_frames
        << ",l" << stats.lpc_rice_frames
        << ",v" << stats.verbatim_frames;
    return out.str();
}

double seconds_from_ns(std::uint64_t ns)
{
    return static_cast<double>(ns) / 1'000'000'000.0;
}

void print_native_stats(const ldcompress::NativeCompressionStats& stats)
{
    std::cerr << "native stats: frames=" << stats.frames
              << " constant=" << stats.constant_frames
              << " fixed-rice=" << stats.fixed_rice_frames
              << " lpc-rice=" << stats.lpc_rice_frames
              << " verbatim=" << stats.verbatim_frames
              << " estimated-subframe-bits=" << stats.estimated_subframe_bits
              << '\n';
    print_nonzero_counts(std::cerr, "fixed orders", stats.fixed_order_counts);
    print_nonzero_counts(std::cerr, "lpc orders", stats.lpc_order_counts);
    print_nonzero_counts(std::cerr, "partition orders", stats.partition_order_counts);
    print_nonzero_counts(std::cerr, "wasted bits", stats.wasted_bits_counts);
    if (stats.accelerated_batches != 0 || stats.accelerated_total_ns != 0) {
        const auto measured_analyzer_parts =
            stats.accelerated_task_plan_ns + stats.accelerated_exact_analysis_ns;
        const auto analyzer_other_ns = stats.accelerated_analyzer_ns > measured_analyzer_parts
            ? stats.accelerated_analyzer_ns - measured_analyzer_parts
            : 0;
        std::cerr << std::fixed << std::setprecision(6)
                  << "accelerated timings: batches=" << stats.accelerated_batches
                  << " total=" << seconds_from_ns(stats.accelerated_total_ns) << "s"
                  << " setup=" << seconds_from_ns(stats.accelerated_setup_ns) << "s"
                  << " ingest=" << seconds_from_ns(stats.accelerated_scan_ns) << "s"
                  << " analyzer=" << seconds_from_ns(stats.accelerated_analyzer_ns) << "s"
                  << " selected-write="
                  << seconds_from_ns(stats.accelerated_selected_write_ns) << "s"
                  << " tail-write=" << seconds_from_ns(stats.accelerated_tail_write_ns) << "s"
                  << '\n';
        const auto scan_detail_ns =
            stats.accelerated_scan_read_ns +
            stats.accelerated_scan_decode_ns +
            stats.accelerated_scan_md5_ns;
        if (scan_detail_ns != 0) {
            const auto scan_other_ns = stats.accelerated_scan_ns > scan_detail_ns
                ? stats.accelerated_scan_ns - scan_detail_ns
                : 0;
            std::cerr << "accelerated scan timings: read="
                      << seconds_from_ns(stats.accelerated_scan_read_ns) << "s"
                      << " decode=" << seconds_from_ns(stats.accelerated_scan_decode_ns)
                      << "s"
                      << " md5=" << seconds_from_ns(stats.accelerated_scan_md5_ns) << "s"
                      << " other=" << seconds_from_ns(scan_other_ns) << "s"
                      << '\n';
        }
        const auto opencl_setup_detail_ns =
            stats.opencl_setup_device_ns +
            stats.opencl_setup_context_ns +
            stats.opencl_setup_queue_ns +
            stats.opencl_setup_program_source_ns +
            stats.opencl_setup_program_build_ns +
            stats.opencl_setup_kernels_ns;
        if (opencl_setup_detail_ns != 0) {
            const auto opencl_setup_other_ns =
                stats.accelerated_setup_ns > opencl_setup_detail_ns
                ? stats.accelerated_setup_ns - opencl_setup_detail_ns
                : 0;
            std::cerr << "opencl setup timings: device="
                      << seconds_from_ns(stats.opencl_setup_device_ns) << "s"
                      << " context=" << seconds_from_ns(stats.opencl_setup_context_ns)
                      << "s"
                      << " queue=" << seconds_from_ns(stats.opencl_setup_queue_ns)
                      << "s"
                      << " program-source="
                      << seconds_from_ns(stats.opencl_setup_program_source_ns) << "s"
                      << " program-build="
                      << seconds_from_ns(stats.opencl_setup_program_build_ns) << "s"
                      << " kernels=" << seconds_from_ns(stats.opencl_setup_kernels_ns)
                      << "s"
                      << " other=" << seconds_from_ns(opencl_setup_other_ns) << "s"
                      << '\n';
        }
        if (stats.accelerated_task_plan_ns != 0 ||
            stats.accelerated_exact_analysis_ns != 0) {
            std::cerr << "accelerator timings: task-plan="
                      << seconds_from_ns(stats.accelerated_task_plan_ns) << "s"
                      << " exact-analysis="
                      << seconds_from_ns(stats.accelerated_exact_analysis_ns) << "s"
                      << " analyzer-other=" << seconds_from_ns(analyzer_other_ns) << "s"
                      << '\n';
        }
        if (stats.opencl_task_plan_fixed_guess_ns != 0 ||
            stats.opencl_task_plan_fill_ns != 0) {
            const auto opencl_plan_detail_ns =
                stats.opencl_task_plan_fixed_guess_ns +
                stats.opencl_task_plan_fill_ns;
            const auto opencl_plan_other_ns =
                stats.accelerated_task_plan_ns > opencl_plan_detail_ns
                ? stats.accelerated_task_plan_ns - opencl_plan_detail_ns
                : 0;
            std::cerr << "opencl task-plan timings: fixed-guess="
                      << seconds_from_ns(stats.opencl_task_plan_fixed_guess_ns) << "s"
                      << " task-fill="
                      << seconds_from_ns(stats.opencl_task_plan_fill_ns) << "s"
                      << " other=" << seconds_from_ns(opencl_plan_other_ns) << "s"
                      << '\n';
        }
        if (stats.opencl_timed_batches != 0) {
            const auto opencl_detail_ns =
                stats.opencl_upload_ns +
                stats.opencl_wasted_bits_ns +
                stats.opencl_generated_autocorrelation_ns +
                stats.opencl_generated_lpc_ns +
                stats.opencl_generated_quantize_ns +
                stats.opencl_fixed_order_guess_ns +
                stats.opencl_exact_analysis_ns +
                stats.opencl_choose_best_ns +
                stats.opencl_readback_ns;
            const auto opencl_other_ns =
                stats.accelerated_exact_analysis_ns > opencl_detail_ns
                ? stats.accelerated_exact_analysis_ns - opencl_detail_ns
                : 0;
            std::cerr << "opencl timings: batches=" << stats.opencl_timed_batches
                      << " upload=" << seconds_from_ns(stats.opencl_upload_ns) << "s"
                      << " wasted=" << seconds_from_ns(stats.opencl_wasted_bits_ns) << "s"
                      << " autocor="
                      << seconds_from_ns(stats.opencl_generated_autocorrelation_ns) << "s"
                      << " lpc=" << seconds_from_ns(stats.opencl_generated_lpc_ns) << "s"
                      << " quantize="
                      << seconds_from_ns(stats.opencl_generated_quantize_ns) << "s"
                      << " fixed-guess="
                      << seconds_from_ns(stats.opencl_fixed_order_guess_ns) << "s"
                      << " exact=" << seconds_from_ns(stats.opencl_exact_analysis_ns)
                      << "s"
                      << " choose=" << seconds_from_ns(stats.opencl_choose_best_ns) << "s"
                      << " readback=" << seconds_from_ns(stats.opencl_readback_ns) << "s"
                      << " other=" << seconds_from_ns(opencl_other_ns) << "s"
                      << '\n';
        }
        const auto selected_detail_ns =
            stats.accelerated_selected_validation_ns +
            stats.accelerated_selected_shift_ns +
            stats.accelerated_selected_residual_ns +
            stats.accelerated_selected_rice_parameter_ns +
            stats.accelerated_selected_bitstream_ns +
            stats.accelerated_selected_frame_output_ns;
        if (selected_detail_ns != 0) {
            const auto selected_other_ns =
                stats.accelerated_selected_write_ns > selected_detail_ns
                ? stats.accelerated_selected_write_ns - selected_detail_ns
                : 0;
            std::cerr << "selected-writer timings: validate="
                      << seconds_from_ns(stats.accelerated_selected_validation_ns) << "s"
                      << " shift=" << seconds_from_ns(stats.accelerated_selected_shift_ns)
                      << "s"
                      << " residual="
                      << seconds_from_ns(stats.accelerated_selected_residual_ns) << "s"
                      << " rice-params="
                      << seconds_from_ns(stats.accelerated_selected_rice_parameter_ns)
                      << "s"
                      << " bitstream="
                      << seconds_from_ns(stats.accelerated_selected_bitstream_ns) << "s"
                      << " frame-output="
                      << seconds_from_ns(stats.accelerated_selected_frame_output_ns)
                      << "s"
                      << " other=" << seconds_from_ns(selected_other_ns) << "s"
                      << '\n';
        }
        if (stats.vulkan_gpu_timed_batches != 0) {
            std::cerr << "vulkan gpu timings: batches=" << stats.vulkan_gpu_timed_batches
                      << " total=" << seconds_from_ns(stats.vulkan_gpu_total_ns) << "s"
                      << " upload=" << seconds_from_ns(stats.vulkan_gpu_upload_ns) << "s"
                      << " prepare="
                      << seconds_from_ns(stats.vulkan_gpu_prepare_ns) << "s"
                      << " autocor="
                      << seconds_from_ns(stats.vulkan_gpu_generated_autocorrelation_ns) << "s"
                      << " lpc=" << seconds_from_ns(stats.vulkan_gpu_generated_lpc_ns) << "s"
                      << " quantize="
                      << seconds_from_ns(stats.vulkan_gpu_generated_quantize_ns) << "s"
                      << " exact="
                      << seconds_from_ns(stats.vulkan_gpu_exact_analysis_ns) << "s"
                      << " choose=" << seconds_from_ns(stats.vulkan_gpu_choose_best_ns) << "s"
                      << " readback="
                      << seconds_from_ns(stats.vulkan_gpu_readback_ns) << "s"
                      << '\n';
        }
        if (stats.metal_timed_batches != 0) {
            std::cerr << "metal timings: batches=" << stats.metal_timed_batches
                      << " upload=" << seconds_from_ns(stats.metal_upload_ns) << "s"
                      << " generated="
                      << seconds_from_ns(stats.metal_generated_total_ns) << "s"
                      << " wasted=" << seconds_from_ns(stats.metal_wasted_bits_ns)
                      << "s"
                      << " autocor="
                      << seconds_from_ns(stats.metal_generated_autocorrelation_ns)
                      << "s"
                      << " lpc=" << seconds_from_ns(stats.metal_generated_lpc_ns) << "s"
                      << " quantize="
                      << seconds_from_ns(stats.metal_generated_quantize_ns) << "s"
                      << " fixed-guess="
                      << seconds_from_ns(stats.metal_fixed_order_guess_ns) << "s"
                      << " exact=" << seconds_from_ns(stats.metal_exact_analysis_ns)
                      << "s"
                      << " choose=" << seconds_from_ns(stats.metal_choose_best_ns) << "s"
                      << " readback=" << seconds_from_ns(stats.metal_readback_ns) << "s"
                      << '\n';
        }
        std::cerr.unsetf(std::ios::floatfield);
    }
}

std::string format_compression_percentage(
    std::uint64_t input_bytes,
    std::uint64_t output_bytes)
{
    if (input_bytes == 0) {
        return "n/a";
    }

    const double percentage = (static_cast<double>(output_bytes) * 100.0) /
        static_cast<double>(input_bytes);
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << percentage << '%';
    return output.str();
}

int run_compress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    CompressionProgressReporter progress_reporter(
        options.show_progress, regular_input_size_or_unknown(options.input));
    ldcompress::CompressionProgressCallback progress_callback;
    if (options.show_progress) {
        progress_reporter.report(0, 0);
        progress_callback = [&progress_reporter](
                                std::uint64_t consumed_input_bytes,
                                std::uint64_t consumed_samples) {
            progress_reporter.report(consumed_input_bytes, consumed_samples);
        };
    }

    ldcompress::NativeCompressionStats native_stats;
    const ldcompress::CompressionOptions compress_options {
        .backend = options.backend,
        .container = options.container,
        .compression_level = options.level,
        .sample_rate = 40000,
        .thread_count = options.threads,
        .native_frame_samples = options.native_frame_samples,
        .native_max_lpc_order = options.native_max_lpc_order,
        .native_lpc_precision = options.native_lpc_precision,
        .native_max_rice_partition_order = options.native_max_rice_partition_order,
        .native_stats = options.show_stats ? &native_stats : nullptr,
        .progress_callback = std::move(progress_callback),
        .opencl_device_index = effective_opencl_device_index(options),
        .vulkan_device_index = effective_vulkan_device_index(options),
        .metal_device_index = effective_metal_device_index(options),
    };
    // Backend writers open their path destructively, so give them a private
    // same-directory staging directory and publish its payload only on success.
    const TemporaryOutput staging_output(options.output);
    ldcompress::ConversionStats stats;
    stats = ldcompress::compress_lds(input, staging_output.payload().string(), compress_options);

    publish_temporary_output(staging_output, options.output, options.overwrite);

    progress_reporter.finish();
    std::cerr << "compressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes ("
              << format_compression_percentage(stats.input_bytes, stats.output_bytes)
              << ", " << stats.samples
              << " samples, " << ldcompress::backend_name(options.backend)
              << " backend, " << options.threads << " thread"
              << (options.threads == 1 ? "" : "s") << ")\n";
    if (options.show_stats) {
        print_native_stats(native_stats);
    }
    return 0;
}

int run_decompress(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    const TemporaryOutput staging_output(options.output);
    ldcompress::ConversionStats stats;
    DecompressionProgressReporter progress_reporter(options.show_progress);
    ldcompress::DecompressionProgressCallback progress_callback;
    if (options.show_progress) {
        progress_callback = [&progress_reporter](
                                std::uint64_t decoded_samples,
                                std::uint64_t total_samples) {
            progress_reporter.report(decoded_samples, total_samples);
        };
    }
    {
        std::ofstream output(staging_output.payload(), std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not open temporary output: " +
                staging_output.payload().string());
        }

        stats = ldcompress::decompress_flac_to_lds(
            options.input, output, std::move(progress_callback));
        output.close();
        if (!output) {
            throw std::runtime_error("failed to finish decompressed output: " +
                staging_output.payload().string());
        }
    }

    publish_temporary_output(staging_output, options.output, options.overwrite);

    progress_reporter.finish();
    std::cerr << "decompressed " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples)\n";
    if (stats.streaminfo_total_samples_underreported) {
        std::cerr << "warning: decoded " << stats.samples << " samples but STREAMINFO declares "
                  << stats.streaminfo_declared_total_samples
                  << "; recovered valid frames through end of stream\n";
    }
    if (stats.streaminfo_pcm_md5_mismatch) {
        std::cerr << "warning: decoded FLAC PCM MD5 did not match STREAMINFO; "
                  << "use verify --source for an end-to-end comparison\n";
    }
    return 0;
}

int run_convert(const Options& options)
{
    ensure_distinct_input_output(options.input, options.output);
    ensure_output_allowed(options.output, options.overwrite);

    std::ifstream input(options.input, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + options.input);
    }

    const TemporaryOutput staging_output(options.output);
    ldcompress::ConversionStats stats;
    {
        std::ofstream output(staging_output.payload(), std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not open temporary output: " +
                staging_output.payload().string());
        }

        stats = options.unpack
            ? ldcompress::unpack_lds10_to_s16le(input, output)
            : ldcompress::pack_s16le_to_lds10(input, output);
        output.close();
        if (!output) {
            throw std::runtime_error("failed to finish converted output: " +
                staging_output.payload().string());
        }
    }

    publish_temporary_output(staging_output, options.output, options.overwrite);

    std::cerr << "converted " << stats.input_bytes << " bytes to "
              << stats.output_bytes << " bytes (" << stats.samples
              << " samples)\n";
    return 0;
}

std::filesystem::path make_bench_temp_dir()
{
    const auto base = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base /
            ("ld-compress-ng-bench-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            throw std::runtime_error("could not create benchmark temp directory: " +
                candidate.string() + ": " + ec.message());
        }
    }
    throw std::runtime_error("could not allocate benchmark temp directory");
}

class ScopedDirectory final {
public:
    explicit ScopedDirectory(std::filesystem::path path) : path_(std::move(path)) {}

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    ~ScopedDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct BenchCase {
    ldcompress::CompressionBackend backend;
    ldcompress::FlacContainer container;
    unsigned threads;
    unsigned native_frame_samples;
    unsigned native_max_lpc_order;
    unsigned native_lpc_precision;
    unsigned native_max_rice_partition_order;
    ldcompress::NativeAnalysisProfile native_analysis_profile;
    std::optional<std::size_t> opencl_device_index;
    std::optional<std::size_t> vulkan_device_index;
    std::optional<std::size_t> metal_device_index;
    bool show_frame_samples;
    bool show_lpc_order;
    bool show_lpc_precision;
    bool show_rice_partition_order;
    bool show_analysis_profile;
};

struct BenchResult {
    const char* backend;
    unsigned threads;
    unsigned native_frame_samples;
    unsigned native_max_lpc_order;
    unsigned native_lpc_precision;
    unsigned native_max_rice_partition_order;
    ldcompress::NativeAnalysisProfile native_analysis_profile;
    bool show_frame_samples;
    bool show_lpc_order;
    bool show_lpc_precision;
    bool show_rice_partition_order;
    bool show_analysis_profile;
    ldcompress::ConversionStats stats;
    ldcompress::NativeCompressionStats native_stats;
    bool show_native_stats;
    double elapsed_seconds;
};

BenchResult run_bench_case(
    const std::string& input_path,
    const std::filesystem::path& output_path,
    const BenchCase& bench_case,
    ldcompress::OpenClCompressionSession* opencl_session = nullptr,
    ldcompress::VulkanCompressionSession* vulkan_session = nullptr,
    ldcompress::MetalCompressionSession* metal_session = nullptr)
{
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + input_path);
    }

    ldcompress::NativeCompressionStats native_stats;
    const bool collect_native_stats = is_native_flac_backend(bench_case.backend);
    const ldcompress::CompressionOptions compress_options {
        .backend = bench_case.backend,
        .container = bench_case.container,
        .compression_level = 11,
        .sample_rate = 40000,
        .thread_count = bench_case.threads,
        .native_frame_samples = bench_case.native_frame_samples,
        .native_max_lpc_order = bench_case.native_max_lpc_order,
        .native_lpc_precision = bench_case.native_lpc_precision,
        .native_max_rice_partition_order = bench_case.native_max_rice_partition_order,
        .native_analysis_profile = bench_case.native_analysis_profile,
        .native_stats = collect_native_stats ? &native_stats : nullptr,
        .progress_callback = {},
        .opencl_device_index = bench_case.opencl_device_index,
        .vulkan_device_index = bench_case.vulkan_device_index,
        .metal_device_index = bench_case.metal_device_index,
    };

    const auto started = std::chrono::steady_clock::now();
    ldcompress::ConversionStats stats;
    if (opencl_session != nullptr &&
        bench_case.backend == ldcompress::CompressionBackend::OpenClNativeFlac) {
        stats = opencl_session->compress_lds_to_native_flac(
            input,
            output_path.string(),
            ldcompress::OpenClCompressionOptions {
                .container = bench_case.container,
                .sample_rate = 40000,
                .thread_count = bench_case.threads,
                .frame_samples = bench_case.native_frame_samples,
                .max_lpc_order = bench_case.native_max_lpc_order,
                .lpc_precision = bench_case.native_lpc_precision,
                .max_rice_partition_order = bench_case.native_max_rice_partition_order,
                .analysis_profile = bench_case.native_analysis_profile,
                .device_index = bench_case.opencl_device_index,
                .native_stats = collect_native_stats ? &native_stats : nullptr,
                .progress_callback = {},
            });
    } else if (vulkan_session != nullptr &&
        bench_case.backend == ldcompress::CompressionBackend::VulkanNativeFlac) {
        stats = vulkan_session->compress_lds_to_native_flac(
            input,
            output_path.string(),
            ldcompress::VulkanCompressionOptions {
                .container = bench_case.container,
                .sample_rate = 40000,
                .thread_count = bench_case.threads,
                .frame_samples = bench_case.native_frame_samples,
                .max_lpc_order = bench_case.native_max_lpc_order,
                .lpc_precision = bench_case.native_lpc_precision,
                .max_rice_partition_order = bench_case.native_max_rice_partition_order,
                .analysis_profile = bench_case.native_analysis_profile,
                .device_index = bench_case.vulkan_device_index,
                .native_stats = collect_native_stats ? &native_stats : nullptr,
                .progress_callback = {},
            });
    } else if (metal_session != nullptr &&
        bench_case.backend == ldcompress::CompressionBackend::MetalNativeFlac) {
        stats = metal_session->compress_lds_to_native_flac(
            input,
            output_path.string(),
            ldcompress::MetalCompressionOptions {
                .container = bench_case.container,
                .sample_rate = 40000,
                .thread_count = bench_case.threads,
                .frame_samples = bench_case.native_frame_samples,
                .max_lpc_order = bench_case.native_max_lpc_order,
                .lpc_precision = bench_case.native_lpc_precision,
                .max_rice_partition_order = bench_case.native_max_rice_partition_order,
                .analysis_profile = bench_case.native_analysis_profile,
                .device_index = bench_case.metal_device_index,
                .native_stats = collect_native_stats ? &native_stats : nullptr,
                .progress_callback = {},
            });
    } else {
        stats = ldcompress::compress_lds(input, output_path.string(), compress_options);
    }
    const auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finished - started;

    return BenchResult {
        .backend = ldcompress::backend_name(bench_case.backend),
        .threads = bench_case.threads,
        .native_frame_samples = bench_case.native_frame_samples,
        .native_max_lpc_order = bench_case.native_max_lpc_order,
        .native_lpc_precision = bench_case.native_lpc_precision,
        .native_max_rice_partition_order = bench_case.native_max_rice_partition_order,
        .native_analysis_profile = bench_case.native_analysis_profile,
        .show_frame_samples = bench_case.show_frame_samples,
        .show_lpc_order = bench_case.show_lpc_order,
        .show_lpc_precision = bench_case.show_lpc_precision,
        .show_rice_partition_order = bench_case.show_rice_partition_order,
        .show_analysis_profile = bench_case.show_analysis_profile,
        .stats = stats,
        .native_stats = native_stats,
        .show_native_stats = collect_native_stats,
        .elapsed_seconds = elapsed.count(),
    };
}

void print_optional_unsigned(unsigned value, bool show_value, int width)
{
    std::cout << std::right << std::setw(width);
    if (show_value) {
        std::cout << value;
    } else {
        std::cout << '-';
    }
}

void print_optional_profile(
    ldcompress::NativeAnalysisProfile profile,
    bool show_value,
    int width)
{
    std::cout << std::right << std::setw(width);
    if (show_value) {
        std::cout << ldcompress::native_analysis_profile_name(profile);
    } else {
        std::cout << '-';
    }
}

void print_seconds_field(std::uint64_t nanoseconds)
{
    std::cout << std::setw(18) << std::fixed << std::setprecision(6)
              << seconds_from_ns(nanoseconds);
}

void print_bench_result(const BenchResult& result)
{
    const double ratio = result.stats.input_bytes == 0
        ? 0.0
        : static_cast<double>(result.stats.output_bytes) /
            static_cast<double>(result.stats.input_bytes);
    const double mib_per_second = result.elapsed_seconds <= 0.0
        ? 0.0
        : (static_cast<double>(result.stats.input_bytes) / (1024.0 * 1024.0)) /
            result.elapsed_seconds;

    std::cout << std::left << std::setw(17) << result.backend
              << std::right << std::setw(9) << result.threads;
    print_optional_unsigned(result.native_frame_samples, result.show_frame_samples, 15);
    print_optional_unsigned(result.native_max_lpc_order, result.show_lpc_order, 10);
    print_optional_unsigned(result.native_lpc_precision, result.show_lpc_precision, 9);
    print_optional_unsigned(result.native_max_rice_partition_order, result.show_rice_partition_order, 11);
    print_optional_profile(result.native_analysis_profile, result.show_analysis_profile, 40);
    std::cout << std::setw(14) << result.stats.input_bytes
              << std::setw(15) << result.stats.output_bytes
              << std::setw(12) << result.stats.samples
              << std::setw(10) << std::fixed << std::setprecision(4) << ratio
              << std::setw(11) << std::fixed << std::setprecision(3) << result.elapsed_seconds
              << std::setw(11) << std::fixed << std::setprecision(2) << mib_per_second
              << std::setw(28) << (result.show_native_stats ? summarize_subframes(result.native_stats) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.lpc_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.partition_order_counts) : "-")
              << std::setw(24) << (result.show_native_stats ? summarize_nonzero_counts(result.native_stats.wasted_bits_counts) : "-");
    print_seconds_field(result.native_stats.accelerated_total_ns);
    print_seconds_field(result.native_stats.accelerated_setup_ns);
    print_seconds_field(result.native_stats.accelerated_scan_ns);
    print_seconds_field(result.native_stats.accelerated_scan_read_ns);
    print_seconds_field(result.native_stats.accelerated_scan_decode_ns);
    print_seconds_field(result.native_stats.accelerated_scan_md5_ns);
    print_seconds_field(result.native_stats.accelerated_analyzer_ns);
    print_seconds_field(result.native_stats.accelerated_task_plan_ns);
    print_seconds_field(result.native_stats.accelerated_exact_analysis_ns);
    print_seconds_field(result.native_stats.opencl_task_plan_fixed_guess_ns);
    print_seconds_field(result.native_stats.opencl_task_plan_fill_ns);
    print_seconds_field(result.native_stats.opencl_setup_device_ns);
    print_seconds_field(result.native_stats.opencl_setup_context_ns);
    print_seconds_field(result.native_stats.opencl_setup_queue_ns);
    print_seconds_field(result.native_stats.opencl_setup_program_source_ns);
    print_seconds_field(result.native_stats.opencl_setup_program_build_ns);
    print_seconds_field(result.native_stats.opencl_setup_kernels_ns);
    print_seconds_field(result.native_stats.accelerated_selected_write_ns);
    print_seconds_field(result.native_stats.accelerated_tail_write_ns);
    print_seconds_field(result.native_stats.accelerated_selected_validation_ns);
    print_seconds_field(result.native_stats.accelerated_selected_shift_ns);
    print_seconds_field(result.native_stats.accelerated_selected_residual_ns);
    print_seconds_field(result.native_stats.accelerated_selected_rice_parameter_ns);
    print_seconds_field(result.native_stats.accelerated_selected_bitstream_ns);
    print_seconds_field(result.native_stats.accelerated_selected_frame_output_ns);
    print_seconds_field(result.native_stats.opencl_upload_ns);
    print_seconds_field(result.native_stats.opencl_wasted_bits_ns);
    print_seconds_field(result.native_stats.opencl_generated_autocorrelation_ns);
    print_seconds_field(result.native_stats.opencl_generated_lpc_ns);
    print_seconds_field(result.native_stats.opencl_generated_quantize_ns);
    print_seconds_field(result.native_stats.opencl_fixed_order_guess_ns);
    print_seconds_field(result.native_stats.opencl_exact_analysis_ns);
    print_seconds_field(result.native_stats.opencl_choose_best_ns);
    print_seconds_field(result.native_stats.opencl_readback_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_total_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_upload_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_prepare_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_generated_autocorrelation_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_generated_lpc_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_generated_quantize_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_fixed_order_guess_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_exact_analysis_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_choose_best_ns);
    print_seconds_field(result.native_stats.vulkan_gpu_readback_ns);
    print_seconds_field(result.native_stats.metal_upload_ns);
    print_seconds_field(result.native_stats.metal_generated_total_ns);
    print_seconds_field(result.native_stats.metal_wasted_bits_ns);
    print_seconds_field(result.native_stats.metal_generated_autocorrelation_ns);
    print_seconds_field(result.native_stats.metal_generated_lpc_ns);
    print_seconds_field(result.native_stats.metal_generated_quantize_ns);
    print_seconds_field(result.native_stats.metal_fixed_order_guess_ns);
    print_seconds_field(result.native_stats.metal_exact_analysis_ns);
    print_seconds_field(result.native_stats.metal_choose_best_ns);
    print_seconds_field(result.native_stats.metal_readback_ns);
    std::cout << '\n';
}

int run_bench(const Options& options)
{
    ScopedDirectory temp_dir(make_bench_temp_dir());

    std::vector<BenchCase> cases {
        {
            .backend = ldcompress::CompressionBackend::CpuLibFlac,
            .container = ldcompress::FlacContainer::Ogg,
            .threads = 1,
            .native_frame_samples = kDefaultNativeFrameSamples,
            .native_max_lpc_order = kDefaultNativeMaxLpcOrder,
            .native_lpc_precision = kDefaultNativeLpcPrecision,
            .native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder,
            .native_analysis_profile = ldcompress::NativeAnalysisProfile::Exact,
            .opencl_device_index = std::nullopt,
            .vulkan_device_index = std::nullopt,
            .metal_device_index = std::nullopt,
            .show_frame_samples = false,
            .show_lpc_order = false,
            .show_lpc_precision = false,
            .show_rice_partition_order = false,
            .show_analysis_profile = false,
        },
    };
    std::optional<std::size_t> bench_opencl_device_index;
    std::optional<std::size_t> bench_vulkan_device_index;
    std::optional<std::size_t> bench_metal_device_index;

    for (const unsigned frame_samples : options.bench_frame_samples) {
        cases.push_back(BenchCase {
            .backend = ldcompress::CompressionBackend::NativeVerbatimFlac,
            .container = ldcompress::FlacContainer::Native,
            .threads = 1,
            .native_frame_samples = frame_samples,
            .native_max_lpc_order = 0,
            .native_lpc_precision = kDefaultNativeLpcPrecision,
            .native_max_rice_partition_order = kDefaultNativeMaxRicePartitionOrder,
            .native_analysis_profile = ldcompress::NativeAnalysisProfile::Exact,
            .opencl_device_index = std::nullopt,
            .vulkan_device_index = std::nullopt,
            .metal_device_index = std::nullopt,
            .show_frame_samples = true,
            .show_lpc_order = false,
            .show_lpc_precision = false,
            .show_rice_partition_order = false,
            .show_analysis_profile = false,
        });
    }

    for (const unsigned frame_samples : options.bench_frame_samples) {
        for (const unsigned lpc_order : options.bench_lpc_orders) {
            for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                    for (const auto profile : options.bench_analysis_profiles) {
                        for (const unsigned threads : options.bench_threads) {
                            cases.push_back(BenchCase {
                                .backend = ldcompress::CompressionBackend::NativeFixedFlac,
                                .container = ldcompress::FlacContainer::Native,
                                .threads = threads,
                                .native_frame_samples = frame_samples,
                                .native_max_lpc_order = lpc_order,
                                .native_lpc_precision = lpc_precision,
                                .native_max_rice_partition_order = rice_partition_order,
                                .native_analysis_profile = profile,
                                .opencl_device_index = std::nullopt,
                                .vulkan_device_index = std::nullopt,
                                .metal_device_index = std::nullopt,
                                .show_frame_samples = true,
                                .show_lpc_order = true,
                                .show_lpc_precision = true,
                                .show_rice_partition_order = true,
                                .show_analysis_profile = true,
                            });
                        }
                    }
                }
            }
        }
    }

    if (options.bench_include_opencl) {
        const auto opencl_device_index =
            available_opencl_device_index(effective_opencl_device_index(options));
        if (opencl_device_index.has_value()) {
            bench_opencl_device_index = opencl_device_index;
            for (const unsigned frame_samples : options.bench_frame_samples) {
                for (const unsigned lpc_order : options.bench_lpc_orders) {
                    for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                        for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                            for (const auto profile : options.bench_analysis_profiles) {
                                for (const unsigned threads : options.bench_threads) {
                                    cases.push_back(BenchCase {
                                        .backend = ldcompress::CompressionBackend::OpenClNativeFlac,
                                        .container = ldcompress::FlacContainer::Native,
                                        .threads = threads,
                                        .native_frame_samples = frame_samples,
                                        .native_max_lpc_order = lpc_order,
                                        .native_lpc_precision = lpc_precision,
                                        .native_max_rice_partition_order = rice_partition_order,
                                        .native_analysis_profile = profile,
                                        .opencl_device_index = opencl_device_index,
                                        .vulkan_device_index = std::nullopt,
                                        .metal_device_index = std::nullopt,
                                        .show_frame_samples = true,
                                        .show_lpc_order = true,
                                        .show_lpc_precision = true,
                                        .show_rice_partition_order = true,
                                        .show_analysis_profile = true,
                                    });
                                }
                            }
                        }
                    }
                }
            }
        } else {
            const auto requested_opencl_device_index = effective_opencl_device_index(options);
            if (requested_opencl_device_index.has_value()) {
                throw std::runtime_error(
                    "bench: requested OpenCL device " +
                    std::to_string(*requested_opencl_device_index) +
                    " is not available");
            } else {
                std::cerr << "bench: OpenCL requested but no available device was found; omitting opencl rows\n";
            }
        }
    }

    if (options.bench_include_vulkan) {
        const auto vulkan_device_index =
            available_vulkan_device_index(effective_vulkan_device_index(options));
        if (vulkan_device_index.has_value()) {
            bench_vulkan_device_index = vulkan_device_index;
            for (const unsigned frame_samples : options.bench_frame_samples) {
                for (const unsigned lpc_order : options.bench_lpc_orders) {
                    for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                        for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                            for (const auto profile : options.bench_analysis_profiles) {
                                for (const unsigned threads : options.bench_threads) {
                                    cases.push_back(BenchCase {
                                        .backend = ldcompress::CompressionBackend::VulkanNativeFlac,
                                        .container = ldcompress::FlacContainer::Native,
                                        .threads = threads,
                                        .native_frame_samples = frame_samples,
                                        .native_max_lpc_order = lpc_order,
                                        .native_lpc_precision = lpc_precision,
                                        .native_max_rice_partition_order = rice_partition_order,
                                        .native_analysis_profile = profile,
                                        .opencl_device_index = std::nullopt,
                                        .vulkan_device_index = vulkan_device_index,
                                        .metal_device_index = std::nullopt,
                                        .show_frame_samples = true,
                                        .show_lpc_order = true,
                                        .show_lpc_precision = true,
                                        .show_rice_partition_order = true,
                                        .show_analysis_profile = true,
                                    });
                                }
                            }
                        }
                    }
                }
            }
        } else {
            const auto requested_vulkan_device_index = effective_vulkan_device_index(options);
            if (requested_vulkan_device_index.has_value()) {
                throw std::runtime_error(
                    "bench: requested Vulkan device " +
                    std::to_string(*requested_vulkan_device_index) +
                    " is not available or lacks shaderInt64");
            } else {
                std::cerr << "bench: Vulkan requested but no non-CPU device with shaderInt64 was found; omitting vulkan rows\n";
            }
        }
    }

    if (options.bench_include_metal) {
        const auto metal_device_index =
            available_metal_device_index(effective_metal_device_index(options));
        if (metal_device_index.has_value()) {
            bench_metal_device_index = metal_device_index;
            for (const unsigned frame_samples : options.bench_frame_samples) {
                for (const unsigned lpc_order : options.bench_lpc_orders) {
                    for (const unsigned lpc_precision : options.bench_lpc_precisions) {
                        for (const unsigned rice_partition_order : options.bench_rice_partition_orders) {
                            for (const auto profile : options.bench_analysis_profiles) {
                                for (const unsigned threads : options.bench_threads) {
                                    cases.push_back(BenchCase {
                                        .backend = ldcompress::CompressionBackend::MetalNativeFlac,
                                        .container = ldcompress::FlacContainer::Native,
                                        .threads = threads,
                                        .native_frame_samples = frame_samples,
                                        .native_max_lpc_order = lpc_order,
                                        .native_lpc_precision = lpc_precision,
                                        .native_max_rice_partition_order = rice_partition_order,
                                        .native_analysis_profile = profile,
                                        .opencl_device_index = std::nullopt,
                                        .vulkan_device_index = std::nullopt,
                                        .metal_device_index = metal_device_index,
                                        .show_frame_samples = true,
                                        .show_lpc_order = true,
                                        .show_lpc_precision = true,
                                        .show_rice_partition_order = true,
                                        .show_analysis_profile = true,
                                    });
                                }
                            }
                        }
                    }
                }
            }
        } else {
            const auto requested_metal_device_index = effective_metal_device_index(options);
            if (requested_metal_device_index.has_value()) {
                throw std::runtime_error(
                    "bench: requested Metal device " +
                    std::to_string(*requested_metal_device_index) +
                    " is not available");
            } else {
                std::cerr << "bench: Metal requested but no available device was found; omitting metal rows\n";
            }
        }
    }

    std::cout << std::left << std::setw(17) << "backend"
              << std::right << std::setw(9) << "threads"
              << std::setw(15) << "frame_samples"
              << std::setw(10) << "lpc_order"
              << std::setw(9) << "lpc_prec"
              << std::setw(11) << "rice_order"
              << std::setw(40) << "profile"
              << std::setw(14) << "input_bytes"
              << std::setw(15) << "output_bytes"
              << std::setw(12) << "samples"
              << std::setw(10) << "ratio"
              << std::setw(11) << "elapsed_s"
              << std::setw(11) << "mib_per_s"
              << std::setw(28) << "subframes"
              << std::setw(24) << "lpc_orders"
              << std::setw(24) << "rice_orders"
              << std::setw(24) << "wasted_bits"
              << std::setw(18) << "accel_total_s"
              << std::setw(18) << "accel_setup_s"
              << std::setw(18) << "accel_scan_s"
              << std::setw(20) << "accel_scan_read_s"
              << std::setw(20) << "accel_scan_decode_s"
              << std::setw(20) << "accel_scan_md5_s"
              << std::setw(18) << "accel_analyze_s"
              << std::setw(18) << "accel_plan_s"
              << std::setw(18) << "accel_exact_s"
              << std::setw(18) << "ocl_plan_guess_s"
              << std::setw(18) << "ocl_plan_fill_s"
              << std::setw(18) << "ocl_setup_dev_s"
              << std::setw(18) << "ocl_setup_ctx_s"
              << std::setw(18) << "ocl_setup_q_s"
              << std::setw(18) << "ocl_setup_src_s"
              << std::setw(18) << "ocl_setup_build_s"
              << std::setw(19) << "ocl_setup_kernel_s"
              << std::setw(18) << "writer_total_s"
              << std::setw(18) << "tail_write_s"
              << std::setw(18) << "writer_val_s"
              << std::setw(18) << "writer_shift_s"
              << std::setw(18) << "writer_resid_s"
              << std::setw(18) << "writer_rice_s"
              << std::setw(18) << "writer_bits_s"
              << std::setw(18) << "writer_out_s"
              << std::setw(18) << "opencl_up_s"
              << std::setw(18) << "opencl_waste_s"
              << std::setw(18) << "opencl_ac_s"
              << std::setw(18) << "opencl_lpc_s"
              << std::setw(18) << "opencl_quant_s"
              << std::setw(18) << "opencl_fguess_s"
              << std::setw(18) << "opencl_exact_s"
              << std::setw(18) << "opencl_choose_s"
              << std::setw(18) << "opencl_read_s"
              << std::setw(18) << "vk_gpu_total_s"
              << std::setw(18) << "vk_gpu_up_s"
              << std::setw(18) << "vk_gpu_prep_s"
              << std::setw(18) << "vk_gpu_ac_s"
              << std::setw(18) << "vk_gpu_lpc_s"
              << std::setw(18) << "vk_gpu_quant_s"
              << std::setw(18) << "vk_gpu_fguess_s"
              << std::setw(18) << "vk_gpu_exact_s"
              << std::setw(18) << "vk_gpu_choose_s"
              << std::setw(18) << "vk_gpu_read_s"
              << std::setw(18) << "metal_up_s"
              << std::setw(18) << "metal_gen_s"
              << std::setw(18) << "metal_waste_s"
              << std::setw(18) << "metal_ac_s"
              << std::setw(18) << "metal_lpc_s"
              << std::setw(18) << "metal_quant_s"
              << std::setw(18) << "metal_fguess_s"
              << std::setw(18) << "metal_exact_s"
              << std::setw(18) << "metal_choose_s"
              << std::setw(18) << "metal_read_s"
              << '\n';

    std::unique_ptr<ldcompress::OpenClCompressionSession> opencl_session;
    if (options.bench_reuse_opencl_session && bench_opencl_device_index.has_value()) {
        opencl_session = std::make_unique<ldcompress::OpenClCompressionSession>(
            bench_opencl_device_index);
    }

    std::unique_ptr<ldcompress::VulkanCompressionSession> vulkan_session;
    if (options.bench_reuse_vulkan_session && bench_vulkan_device_index.has_value()) {
        vulkan_session = std::make_unique<ldcompress::VulkanCompressionSession>(
            bench_vulkan_device_index);
    }

    std::unique_ptr<ldcompress::MetalCompressionSession> metal_session;
    if (options.bench_reuse_metal_session && bench_metal_device_index.has_value()) {
        metal_session = std::make_unique<ldcompress::MetalCompressionSession>(
            bench_metal_device_index);
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto output_path = temp_dir.path() /
            ("case-" + std::to_string(i) +
                (cases[i].container == ldcompress::FlacContainer::Native ? ".flac.ldf" : ".ldf"));
        print_bench_result(run_bench_case(
            options.input,
            output_path,
            cases[i],
            opencl_session.get(),
            vulkan_session.get(),
            metal_session.get()));
    }

    return 0;
}

int run_verify(const Options& options)
{
    HashingStreambuf decoded_hash_buffer;
    std::ostream decoded_hash_stream(&decoded_hash_buffer);
    ldcompress::FileDigest compressed;
    const auto decoded_stats = ldcompress::decompress_flac_to_lds_with_input_digest(
        options.input, decoded_hash_stream, compressed);
    decoded_hash_stream.flush();
    if (!decoded_hash_stream) {
        throw std::runtime_error("failed to hash decoded stream");
    }
    if (decoded_stats.streaminfo_total_samples_underreported) {
        std::cerr << "warning: decoded " << decoded_stats.samples
                  << " samples but STREAMINFO declares "
                  << decoded_stats.streaminfo_declared_total_samples
                  << "; recovered valid frames through end of stream\n";
    }
    if (decoded_stats.streaminfo_pcm_md5_mismatch) {
        std::cerr << "warning: decoded FLAC PCM MD5 did not match STREAMINFO; "
                  << "use verify --source for an end-to-end comparison\n";
    }

    std::cout << "compressed md5 " << compressed.md5.hex()
              << "  " << compressed.bytes << " bytes  " << options.input << '\n';
    std::cout << "decoded    md5 " << decoded_hash_buffer.md5().hex()
              << "  " << decoded_hash_buffer.bytes() << " bytes  "
              << default_decompress_output(options.input) << '\n';

    if (!options.source.empty()) {
        const auto source = ldcompress::md5_file(options.source);
        const bool match = source.bytes == decoded_hash_buffer.bytes() &&
            source.md5.digest() == decoded_hash_buffer.md5().digest();
        std::cout << "source     md5 " << source.md5.hex()
                  << "  " << source.bytes << " bytes  " << options.source << '\n';
        std::cout << "source comparison: " << (match ? "match" : "mismatch") << '\n';
        return match ? 0 : 1;
    }

    (void)decoded_stats;
    return 0;
}

int run_devices(int argc, char** argv)
{
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            usage(0);
        }
        throw std::runtime_error("unknown option for devices: " + std::string(arg));
    }

    if (!ldcompress::opencl_support_built()) {
        std::cout << "OpenCL support: not built\n";
    } else {
        std::cout << "OpenCL support: built\n";
        const auto devices = ldcompress::list_opencl_devices();
        if (devices.empty()) {
            std::cout << "No OpenCL devices found\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                std::cout << '[' << device.flat_index << "] " << device.device_name << '\n'
                          << "    platform/device index: " << device.platform_index << '/'
                          << device.device_index << '\n'
                          << "    type: " << device.type << '\n'
                          << "    available: " << (device.available ? "yes" : "no") << '\n'
                          << "    compute units: " << device.compute_units << '\n'
                          << "    global memory: " << device.global_memory_bytes << " bytes\n"
                          << "    vendor: " << device.device_vendor << '\n'
                          << "    device version: " << device.device_version << '\n'
                          << "    driver version: " << device.driver_version << '\n'
                          << "    platform: " << device.platform_name << '\n'
                          << "    platform vendor: " << device.platform_vendor << '\n'
                          << "    platform version: " << device.platform_version << '\n';
            }
        }
    }

    std::cout << '\n';
    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan support: not built\n";
    } else {
        std::cout << "Vulkan support: built\n";
        const auto devices = ldcompress::list_vulkan_devices();
        if (devices.empty()) {
            std::cout << "No Vulkan devices found\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                std::cout << '[' << device.index << "] " << device.device_name << '\n'
                          << "    type: " << device.device_type << '\n'
                          << "    available: " << (device.available ? "yes" : "no") << '\n'
                          << "    compute queue family: " << device.compute_queue_family_index << '\n'
                          << "    compute queues: " << device.compute_queue_count << '\n'
                          << "    shaderInt64: " << (device.shader_int64 ? "yes" : "no") << '\n'
                          << "    vulkan backend usable: "
                          << (device.available && device.shader_int64 ? "yes" : "no") << '\n'
                          << "    max workgroup invocations: "
                          << device.max_compute_work_group_invocations << '\n'
                          << "    max compute shared memory: "
                          << device.max_compute_shared_memory_bytes << " bytes\n"
                          << "    device-local memory: "
                          << device.device_local_memory_bytes << " bytes\n"
                          << "    vendor: " << device.vendor_name << '\n'
                          << "    vendor id: 0x" << std::hex << std::setw(4)
                          << std::setfill('0') << device.vendor_id
                          << std::dec << std::setfill(' ') << '\n'
                          << "    device id: 0x" << std::hex << std::setw(4)
                          << std::setfill('0') << device.device_id
                          << std::dec << std::setfill(' ') << '\n'
                          << "    api version: " << device.api_version << '\n'
                          << "    driver version: " << device.driver_version << '\n';
            }
        }
    }

    std::cout << '\n';
    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal support: not built\n";
    } else {
        std::cout << "Metal support: built\n";
        const auto devices = ldcompress::list_metal_devices();
        if (devices.empty()) {
            std::cout << "No Metal devices found\n";
        } else {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                std::cout << '[' << device.index << "] " << device.device_name << '\n'
                          << "    available: " << (device.available ? "yes" : "no") << '\n'
                          << "    low power: " << (device.low_power ? "yes" : "no") << '\n'
                          << "    removable: " << (device.removable ? "yes" : "no") << '\n'
                          << "    unified memory: "
                          << (device.unified_memory ? "yes" : "no") << '\n'
                          << "    recommended max working set: "
                          << device.recommended_max_working_set_bytes << " bytes\n"
                          << "    max buffer length: "
                          << device.max_buffer_length << " bytes\n"
                          << "    max threads per threadgroup: "
                          << device.max_threads_per_threadgroup << '\n'
                          << "    registry id: 0x" << std::hex << std::setw(16)
                          << std::setfill('0') << device.registry_id
                          << std::dec << std::setfill(' ') << '\n';
            }
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        install_staging_output_termination_cleanup();
        if (argc < 2) {
            usage(2);
        }

        const std::string_view command(argv[1]);
        if (command == "--help" || command == "-h") {
            usage(0);
        }
        if (command == "--version") {
            return version();
        }
        if (command == "compress") {
            return run_compress(parse_compress(argc, argv));
        }
        if (command == "decompress") {
            return run_decompress(parse_decompress(argc, argv));
        }
        if (command == "verify") {
            return run_verify(parse_verify(argc, argv));
        }
        if (command == "convert") {
            return run_convert(parse_convert(argc, argv));
        }
        if (command == "bench") {
            return run_bench(parse_bench(argc, argv));
        }
        if (command == "devices") {
            return run_devices(argc, argv);
        }

        throw std::runtime_error("unsupported command for this implementation slice: " +
            std::string(command));
    } catch (const std::exception& ex) {
        std::cerr << "ld-compress-ng: " << ex.what() << '\n';
        return 1;
    }
}
