#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ldcompress {

class BitWriter {
public:
    void write_bits(std::uint64_t value, unsigned bit_count);
    void write_signed(std::int64_t value, unsigned bit_count);
    void write_unary(unsigned zero_count);
    void write_rice_signed_block(std::span<const std::int64_t> values, unsigned parameter);
    void align_zero();
    void clear();
    void reserve_bits(std::size_t bit_count);

    bool byte_aligned() const;
    std::size_t bit_count() const { return bit_count_; }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    void write_zero_bits(std::size_t bit_count);

    std::vector<std::uint8_t> bytes_;
    std::size_t bit_count_ = 0;
};

class FlacCrc8 {
public:
    void update(const std::uint8_t* data, std::size_t size);
    std::uint8_t value() const { return crc_; }

private:
    std::uint8_t crc_ = 0;
};

class FlacCrc16 {
public:
    void update(const std::uint8_t* data, std::size_t size);
    std::uint16_t value() const { return crc_; }

private:
    std::uint16_t crc_ = 0;
};

std::uint8_t flac_crc8(const std::uint8_t* data, std::size_t size);
std::uint16_t flac_crc16(const std::uint8_t* data, std::size_t size);

}  // namespace ldcompress
