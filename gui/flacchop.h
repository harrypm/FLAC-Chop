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
    int32_t total_samples_scanned;   // 1 if total was obtained by scanning frame headers
    int32_t total_samples_from_companion; // 1 if total was inferred from a sibling .log/.wav
    int32_t total_samples_from_vorbis;   // 1 if total was read from a Vorbis RF_TOTAL_SAMPLES tag
    int32_t rate_from_vorbis;            // 1 if RF rate was confirmed by a Vorbis RF_SAMPLE_RATE tag
    uint32_t bits_per_sample;
    uint32_t channels;
    uint64_t file_size;
    uint64_t audio_offset;
    double real_rate_hz;           // real rate in Hz (header*1000 for RF, or header for audio)
    int32_t is_rf;                 // 1 if treated as RF (rate was x1000 or msps hint used)
    double msps;
    int32_t msps_known;
    char error[256];
    // Non-fatal diagnostics (tag-unit corrections, scan misalignment, vorbis
    // mismatches), "; "-joined. Empty when everything checked out.
    char warnings[512];
    // Sniffed input container format (appended at the end — ABI-append-only):
    // 0=flac 1=wav 2=u8(raw) 3=s8(raw) 4=u16(raw) 5=s16(raw).
    uint32_t format;
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
    char stderr_buf[1024];
};

void fc_probe(const char* path, FcProbe* out);
void fc_plan(double start_sec, double len_sec, double real_rate_hz,
             uint64_t total_samples, int32_t total_known, FcPlan* out);
void fc_chop(const char* in_path, const char* out_path,
             uint64_t start_samples, uint64_t length_samples,
             uint64_t output_rate_hz, uint32_t output_bits,
             int32_t basic_rf_filter, int32_t is_rf, FcChopResult* out);
int fc_generate_output_path(const char* in_path, const char* out_dir,
                            const char* stem, char* out_buf, uintptr_t buf_len);
int fc_sox_available(void);
void fc_chop_cancel(void);

// --- Metadata editor (GUI Metadata Editor tab) -----------------------------
// Read/rewrite every Vorbis comment on a *source* FLAC in place. The read path
// packs comments into a little-endian blob: u32 LE count, then per comment
// u32 LE length + "KEY=value" bytes. The vendor string is preserved on write.
//
// fc_comments_blob_size returns the blob byte size (>= 4 on success, 0 on
// error with a message in err). fc_read_comments_blob fills buf (>= that
// size) and returns 1 on success, 0 on error. fc_replace_comments replaces
// ALL comments with the n NUL-terminated "KEY=value" strings in `comments`
// (vendor preserved); returns 1 on success, 0 on error (message in err).
uintptr_t fc_comments_blob_size(const char* path, char* err, uintptr_t err_len);
int fc_read_comments_blob(const char* path, char* buf, uintptr_t buf_len,
                          char* err, uintptr_t err_len);
int fc_replace_comments(const char* path, const char* const* comments,
                        uint32_t n, char* err, uintptr_t err_len);

// Compute the standard RF Vorbis-tag template (RF_SAMPLE_RATE_KHZ,
// RF_SAMPLE_RATE, RF_TOTAL_SAMPLES, DURATION_SECONDS, LENGTH) from the probe
// context, for a tagless RF FLAC. Packs into `buf` in the same blob format as
// fc_read_comments_blob (u32 LE count + per-comment u32 LE len + "KEY=value").
// Returns bytes written (>= 4; a non-RF probe yields a 4-byte count=0 blob) on
// success, 0 on error (null pointers / buffer too small — message in `err`).
// A 4096-byte buffer is always enough. The GUI merges the pairs into the
// editor table (only missing keys) and the user hits Save to write.
uintptr_t fc_rf_template_from_probe(const FcProbe* probe, char* buf, uintptr_t buf_len,
                                    char* err, uintptr_t err_len);

#ifdef __cplusplus
}
#endif

#endif // FLACCHOP_FFI_H
