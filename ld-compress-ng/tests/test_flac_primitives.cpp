#include "flac_primitives.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_bytes(
    const std::vector<std::uint8_t>& actual,
    const std::vector<std::uint8_t>& expected,
    const char* label)
{
    if (actual != expected) {
        throw std::runtime_error(std::string(label) + ": byte vector mismatch");
    }
}

std::uint64_t fold_signed_rice_value(std::int64_t value)
{
    if (value >= 0) {
        return static_cast<std::uint64_t>(value) << 1U;
    }
    return (static_cast<std::uint64_t>(-(value + 1)) << 1U) + 1U;
}

void write_scalar_rice_signed(
    ldcompress::BitWriter& writer,
    std::int64_t value,
    unsigned parameter)
{
    const auto folded = fold_signed_rice_value(value);
    writer.write_unary(static_cast<unsigned>(folded >> parameter));
    if (parameter != 0) {
        writer.write_bits(folded, parameter);
    }
}

void test_bit_writer()
{
    ldcompress::BitWriter writer;
    require(writer.byte_aligned(), "new writer is not byte aligned");

    writer.write_bits(0b101, 3);
    writer.write_bits(0b11110000, 8);
    writer.align_zero();
    require(writer.byte_aligned(), "writer did not align");
    require(writer.bit_count() == 16, "unexpected aligned bit count");
    require_bytes(writer.bytes(), {0xbe, 0x00}, "cross-byte write");

    writer.clear();
    writer.write_bits(0x3ffe, 14);
    writer.write_bits(0, 1);
    writer.write_bits(0, 1);
    writer.align_zero();
    require_bytes(writer.bytes(), {0xff, 0xf8}, "FLAC frame sync prefix");

    writer.clear();
    writer.write_signed(-1, 4);
    writer.write_signed(-8, 4);
    require(writer.byte_aligned(), "signed writes are not byte aligned");
    require_bytes(writer.bytes(), {0xf8}, "signed writes");

    writer.clear();
    writer.write_unary(3);
    writer.align_zero();
    require_bytes(writer.bytes(), {0x10}, "unary write");

    writer.clear();
    writer.reserve_bits(4096);
    writer.write_bits(0x0123456789abcdefULL, 64);
    require(writer.byte_aligned(), "64-bit write is not byte aligned");
    require_bytes(writer.bytes(),
        {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
        "64-bit write");

    writer.clear();
    writer.write_bits(0b11, 2);
    writer.write_unary(14);
    writer.align_zero();
    require_bytes(writer.bytes(), {0xc0, 0x00, 0x80}, "long unary write");

    bool threw = false;
    try {
        writer.write_bits(0, 65);
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "write_bits accepted more than 64 bits");
}

void test_rice_block_writer()
{
    const std::vector<std::int64_t> values {
        0, -1, 1, -2, 2, 7, -8, 64, -65, 511, -512, 4096, -4097,
    };

    for (unsigned parameter = 0; parameter <= 14; ++parameter) {
        ldcompress::BitWriter scalar;
        ldcompress::BitWriter block;
        scalar.write_bits(0b101, 3);
        block.write_bits(0b101, 3);
        for (const auto value : values) {
            write_scalar_rice_signed(scalar, value, parameter);
        }
        block.write_rice_signed_block(values, parameter);
        require(scalar.bit_count() == block.bit_count(),
            "Rice block writer bit count mismatch");
        require_bytes(block.bytes(), scalar.bytes(), "Rice block writer");
    }

    ldcompress::BitWriter scalar;
    ldcompress::BitWriter block;
    const std::vector<std::int64_t> long_unary {8192, -8193};
    for (const auto value : long_unary) {
        write_scalar_rice_signed(scalar, value, 0);
    }
    block.write_rice_signed_block(long_unary, 0);
    require(scalar.bit_count() == block.bit_count(),
        "long Rice block writer bit count mismatch");
    require_bytes(block.bytes(), scalar.bytes(), "long Rice block writer");

    scalar.clear();
    block.clear();
    const std::vector<std::int64_t> boundary {-32, -33};
    for (const auto value : boundary) {
        write_scalar_rice_signed(scalar, value, 0);
    }
    block.write_rice_signed_block(boundary, 0);
    require(scalar.bit_count() == block.bit_count(),
        "boundary Rice block writer bit count mismatch");
    require_bytes(block.bytes(), scalar.bytes(), "boundary Rice block writer");

    block.clear();
    block.write_bits(0b101010, 6);
    block.write_rice_signed_block(std::span<const std::int64_t> {}, 4);
    require(block.bit_count() == 6, "empty Rice block changed bit count");
    require_bytes(block.bytes(), {0xa8}, "empty Rice block changed bytes");

    bool threw = false;
    const std::vector<std::int64_t> invalid_parameter {0};
    try {
        block.write_rice_signed_block(invalid_parameter, 64);
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "Rice block writer accepted parameter 64");

    threw = false;
    const std::vector<std::int64_t> huge_quotient {std::int64_t {1} << 40U};
    try {
        block.write_rice_signed_block(huge_quotient, 0);
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "Rice block writer accepted huge quotient");
}

void test_flac_crcs()
{
    constexpr std::string_view check_input = "123456789";
    const auto* check_bytes = reinterpret_cast<const std::uint8_t*>(check_input.data());
    require(ldcompress::flac_crc8(check_bytes, check_input.size()) == 0xf4,
        "FLAC CRC-8 check vector mismatch");
    require(ldcompress::flac_crc16(check_bytes, check_input.size()) == 0xfee8,
        "FLAC CRC-16 check vector mismatch");

    ldcompress::FlacCrc8 chunked_crc8;
    chunked_crc8.update(check_bytes, 4);
    chunked_crc8.update(check_bytes + 4, check_input.size() - 4);
    require(chunked_crc8.value() == 0xf4, "chunked FLAC CRC-8 mismatch");

    ldcompress::FlacCrc16 chunked_crc16;
    chunked_crc16.update(check_bytes, 4);
    chunked_crc16.update(check_bytes + 4, check_input.size() - 4);
    require(chunked_crc16.value() == 0xfee8, "chunked FLAC CRC-16 mismatch");

    constexpr std::array<std::uint8_t, 7> frame_header_without_crc {
        0xff, 0xf8, 0x6c, 0x08, 0x00, 0x07, 0x28,
    };
    require(ldcompress::flac_crc8(
                frame_header_without_crc.data(), frame_header_without_crc.size()) == 0x0d,
        "FLAC frame header CRC-8 mismatch");

    constexpr std::array<std::uint8_t, 18> frame_without_footer {
        0xff, 0xf8, 0x6c, 0x08, 0x00, 0x07, 0x28, 0x0d, 0x11,
        0x04, 0x10, 0xf8, 0x1f, 0xf8, 0x0c, 0x00, 0x7f, 0x80,
    };
    require(ldcompress::flac_crc16(
                frame_without_footer.data(), frame_without_footer.size()) == 0x9aa2,
        "FLAC frame CRC-16 mismatch");

    require(ldcompress::flac_crc8(nullptr, 0) == 0, "empty CRC-8 mismatch");
    require(ldcompress::flac_crc16(nullptr, 0) == 0, "empty CRC-16 mismatch");
}

}  // namespace

int main()
{
    try {
        test_bit_writer();
        test_rice_block_writer();
        test_flac_crcs();
    } catch (const std::exception& ex) {
        std::cerr << "test_flac_primitives: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
