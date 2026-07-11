#ifndef FLACCHOP_FFI_H
#define FLACCHOP_FFI_H

// C ABI surface produced by the Rust staticlib `flac_chop_core`.
// Field order and types must match the `#[repr(C)]` structs in core/src/ffi.rs
// exactly. Keep these in sync when editing either side.

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

struct FcProbe {
    int32_t ok;
    uint64_t header_sample_rate;
    uint64_t declared_total_samples; // raw STREAMINFO, pre-wrap-correction
    uint64_t total_samples;          // after 36-bit wrap correction
    int32_t total_samples_known;
    uint32_t total_samples_wraps;    // # of 2^36 blocks added (0 = trusted)
    int32_t total_samples_estimated; // 1 if wrap count is an estimate
    uint32_t bits_per_sample;
    uint32_t channels;
    uint64_t file_size;
    uint64_t audio_offset;
    double msps;
    int32_t msps_known;
    char error[256];
};

struct FcPlan {
    int32_t ok;
    uint64_t start_samples;
    uint64_t length_samples;
    uint64_t end_sample;
    double real_sample_rate_hz;
    double real_total_seconds;
    char error[256];
};

struct FcChopResult {
    int32_t ok;
    int32_t exit_code;
    char stderr[1024];
};

void fc_probe(const char* path, FcProbe* out);
void fc_plan(double start_sec, double len_sec, double msps, int32_t msps_known,
             uint64_t header_rate, uint64_t total_samples, int32_t total_known,
             FcPlan* out);
void fc_chop(const char* in_path, const char* out_path,
             uint64_t start_samples, uint64_t length_samples, FcChopResult* out);
int fc_generate_output_path(const char* in_path, char* out_buf, uintptr_t buf_len);
int fc_sox_available(void);

#ifdef __cplusplus
}
#endif

#endif // FLACCHOP_FFI_H
