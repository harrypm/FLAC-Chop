#pragma once

#include <array>
#include <cstdint>
#include <string>

#ifndef LDCOMPRESS_MD5_USE_COMMONCRYPTO
#if defined(__APPLE__) && __has_include(<CommonCrypto/CommonDigest.h>)
#define LDCOMPRESS_MD5_USE_COMMONCRYPTO 1
#else
#define LDCOMPRESS_MD5_USE_COMMONCRYPTO 0
#endif
#endif

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
#include <CommonCrypto/CommonDigest.h>
#endif

namespace ldcompress {

class Md5 {
public:
    Md5();

    void update(const void* data, std::uint64_t size);
    std::array<std::uint8_t, 16> digest() const;
    std::string hex() const;

private:
#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
    void finish();

    CC_MD5_CTX context_ {};
    std::array<std::uint8_t, 16> digest_ {};
    bool finalized_ = false;
#else
    void transform(const std::uint8_t* block);
    void finish();

    std::array<std::uint32_t, 4> state_ {
        0x67452301U,
        0xefcdab89U,
        0x98badcfeU,
        0x10325476U,
    };
    std::array<std::uint8_t, 64> buffer_ {};
    std::uint64_t bytes_ = 0;
    std::size_t buffer_size_ = 0;
    bool finalized_ = false;
#endif
};

struct FileDigest {
    std::uint64_t bytes = 0;
    Md5 md5;
};

FileDigest md5_file(const std::string& path);

}  // namespace ldcompress
