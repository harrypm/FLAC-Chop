#include "hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ldcompress {
namespace {

#if !LDCOMPRESS_MD5_USE_COMMONCRYPTO
constexpr std::array<std::uint32_t, 64> kMd5RoundConstants {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
    0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
    0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
    0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
    0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
    0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
    0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
};

constexpr std::array<unsigned, 64> kMd5Shifts {
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U,
    5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U,
};

std::uint32_t rotate_left(std::uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32U - bits));
}

std::uint32_t read_le32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8U) |
        (static_cast<std::uint32_t>(data[2]) << 16U) |
        (static_cast<std::uint32_t>(data[3]) << 24U);
}

void write_le32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void write_le64(std::uint8_t* data, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        data[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
    }
}
#endif

}  // namespace

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

Md5::Md5()
{
#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
    CC_MD5_Init(&context_);
#endif
}

void Md5::update(const void* data, std::uint64_t size)
{
    if (finalized_) {
        throw std::runtime_error("cannot update finalized MD5 digest");
    }
    if (size == 0) {
        return;
    }

    const auto* input = static_cast<const std::uint8_t*>(data);

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
    while (size != 0) {
        const auto chunk = std::min<std::uint64_t>(
            size, static_cast<std::uint64_t>(std::numeric_limits<CC_LONG>::max()));
        CC_MD5_Update(&context_, input, static_cast<CC_LONG>(chunk));
        input += chunk;
        size -= chunk;
    }
    return;
#else
    bytes_ += size;

    std::uint64_t offset = 0;
    if (buffer_size_ != 0) {
        const auto needed = buffer_.size() - buffer_size_;
        const auto copied = std::min<std::uint64_t>(needed, size);
        std::memcpy(buffer_.data() + buffer_size_, input, static_cast<std::size_t>(copied));
        buffer_size_ += static_cast<std::size_t>(copied);
        offset += copied;

        if (buffer_size_ == buffer_.size()) {
            transform(buffer_.data());
            buffer_size_ = 0;
        }
    }

    while ((size - offset) >= buffer_.size()) {
        transform(input + offset);
        offset += buffer_.size();
    }

    const auto remaining = static_cast<std::size_t>(size - offset);
    if (remaining != 0) {
        std::memcpy(buffer_.data(), input + offset, remaining);
        buffer_size_ = remaining;
    }
#endif
}

std::array<std::uint8_t, 16> Md5::digest() const
{
    Md5 copy(*this);
    copy.finish();

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
    return copy.digest_;
#else
    std::array<std::uint8_t, 16> result {};
    for (std::size_t i = 0; i < copy.state_.size(); ++i) {
        write_le32(result.data() + (i * 4), copy.state_[i]);
    }
    return result;
#endif
}

std::string Md5::hex() const
{
    const auto bytes = digest();
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
static_assert(sizeof(CC_LONG) <= sizeof(std::uint64_t));
#else
void Md5::transform(const std::uint8_t* block)
{
    std::array<std::uint32_t, 16> words {};
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = read_le32(block + (i * 4));
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];

    // The MD5 schedule is fixed. Keeping its indices and rotations compile-time
    // constants avoids per-round dispatch and variable-count rotates.
#define MD5_STEP(expression, target, addend, word_index, round_index) \
    do { \
        target += (expression) + kMd5RoundConstants[round_index] + words[word_index]; \
        target = addend + rotate_left(target, kMd5Shifts[round_index]); \
    } while (false)

    MD5_STEP((b & c) | (~b & d), a, b, 0, 0);
    MD5_STEP((a & b) | (~a & c), d, a, 1, 1);
    MD5_STEP((d & a) | (~d & b), c, d, 2, 2);
    MD5_STEP((c & d) | (~c & a), b, c, 3, 3);
    MD5_STEP((b & c) | (~b & d), a, b, 4, 4);
    MD5_STEP((a & b) | (~a & c), d, a, 5, 5);
    MD5_STEP((d & a) | (~d & b), c, d, 6, 6);
    MD5_STEP((c & d) | (~c & a), b, c, 7, 7);
    MD5_STEP((b & c) | (~b & d), a, b, 8, 8);
    MD5_STEP((a & b) | (~a & c), d, a, 9, 9);
    MD5_STEP((d & a) | (~d & b), c, d, 10, 10);
    MD5_STEP((c & d) | (~c & a), b, c, 11, 11);
    MD5_STEP((b & c) | (~b & d), a, b, 12, 12);
    MD5_STEP((a & b) | (~a & c), d, a, 13, 13);
    MD5_STEP((d & a) | (~d & b), c, d, 14, 14);
    MD5_STEP((c & d) | (~c & a), b, c, 15, 15);

    MD5_STEP((b & d) | (c & ~d), a, b, 1, 16);
    MD5_STEP((a & c) | (b & ~c), d, a, 6, 17);
    MD5_STEP((d & b) | (a & ~b), c, d, 11, 18);
    MD5_STEP((c & a) | (d & ~a), b, c, 0, 19);
    MD5_STEP((b & d) | (c & ~d), a, b, 5, 20);
    MD5_STEP((a & c) | (b & ~c), d, a, 10, 21);
    MD5_STEP((d & b) | (a & ~b), c, d, 15, 22);
    MD5_STEP((c & a) | (d & ~a), b, c, 4, 23);
    MD5_STEP((b & d) | (c & ~d), a, b, 9, 24);
    MD5_STEP((a & c) | (b & ~c), d, a, 14, 25);
    MD5_STEP((d & b) | (a & ~b), c, d, 3, 26);
    MD5_STEP((c & a) | (d & ~a), b, c, 8, 27);
    MD5_STEP((b & d) | (c & ~d), a, b, 13, 28);
    MD5_STEP((a & c) | (b & ~c), d, a, 2, 29);
    MD5_STEP((d & b) | (a & ~b), c, d, 7, 30);
    MD5_STEP((c & a) | (d & ~a), b, c, 12, 31);

    MD5_STEP(b ^ c ^ d, a, b, 5, 32);
    MD5_STEP(a ^ b ^ c, d, a, 8, 33);
    MD5_STEP(d ^ a ^ b, c, d, 11, 34);
    MD5_STEP(c ^ d ^ a, b, c, 14, 35);
    MD5_STEP(b ^ c ^ d, a, b, 1, 36);
    MD5_STEP(a ^ b ^ c, d, a, 4, 37);
    MD5_STEP(d ^ a ^ b, c, d, 7, 38);
    MD5_STEP(c ^ d ^ a, b, c, 10, 39);
    MD5_STEP(b ^ c ^ d, a, b, 13, 40);
    MD5_STEP(a ^ b ^ c, d, a, 0, 41);
    MD5_STEP(d ^ a ^ b, c, d, 3, 42);
    MD5_STEP(c ^ d ^ a, b, c, 6, 43);
    MD5_STEP(b ^ c ^ d, a, b, 9, 44);
    MD5_STEP(a ^ b ^ c, d, a, 12, 45);
    MD5_STEP(d ^ a ^ b, c, d, 15, 46);
    MD5_STEP(c ^ d ^ a, b, c, 2, 47);

    MD5_STEP(c ^ (b | ~d), a, b, 0, 48);
    MD5_STEP(b ^ (a | ~c), d, a, 7, 49);
    MD5_STEP(a ^ (d | ~b), c, d, 14, 50);
    MD5_STEP(d ^ (c | ~a), b, c, 5, 51);
    MD5_STEP(c ^ (b | ~d), a, b, 12, 52);
    MD5_STEP(b ^ (a | ~c), d, a, 3, 53);
    MD5_STEP(a ^ (d | ~b), c, d, 10, 54);
    MD5_STEP(d ^ (c | ~a), b, c, 1, 55);
    MD5_STEP(c ^ (b | ~d), a, b, 8, 56);
    MD5_STEP(b ^ (a | ~c), d, a, 15, 57);
    MD5_STEP(a ^ (d | ~b), c, d, 6, 58);
    MD5_STEP(d ^ (c | ~a), b, c, 13, 59);
    MD5_STEP(c ^ (b | ~d), a, b, 4, 60);
    MD5_STEP(b ^ (a | ~c), d, a, 11, 61);
    MD5_STEP(a ^ (d | ~b), c, d, 2, 62);
    MD5_STEP(d ^ (c | ~a), b, c, 9, 63);

#undef MD5_STEP

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}
#endif

void Md5::finish()
{
    if (finalized_) {
        return;
    }

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO
    CC_MD5_Final(digest_.data(), &context_);
    finalized_ = true;
#else
    const auto original_bits = bytes_ * 8U;

    std::array<std::uint8_t, 64> padding {};
    padding[0] = 0x80U;
    const std::uint64_t padding_size =
        buffer_size_ < 56 ? 56U - buffer_size_ : 120U - buffer_size_;
    update(padding.data(), padding_size);

    std::array<std::uint8_t, 8> length_bytes {};
    write_le64(length_bytes.data(), original_bits);
    update(length_bytes.data(), length_bytes.size());

    finalized_ = true;
#endif
}

#if LDCOMPRESS_MD5_USE_COMMONCRYPTO && defined(__clang__)
#pragma clang diagnostic pop
#endif

FileDigest md5_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + path);
    }

    FileDigest digest;
    std::array<char, 64 * 1024> buffer {};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        digest.md5.update(buffer.data(), static_cast<std::uint64_t>(got));
        digest.bytes += static_cast<std::uint64_t>(got);
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read input: " + path);
    }
    return digest;
}

}  // namespace ldcompress
