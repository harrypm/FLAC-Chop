#include "hash.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require_equal(const std::string& actual, const std::string& expected, const char* label)
{
    if (actual != expected) {
        throw std::runtime_error(std::string(label) + ": expected " + expected + ", got " + actual);
    }
}

std::string md5_hex_chunked(std::string_view input)
{
    ldcompress::Md5 md5;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto chunk_size = std::min<std::size_t>(7, input.size() - offset);
        md5.update(input.data() + offset, chunk_size);
        offset += chunk_size;
    }
    return md5.hex();
}

void test_md5_vectors()
{
    require_equal(md5_hex_chunked(""), "d41d8cd98f00b204e9800998ecf8427e", "empty MD5");
    require_equal(md5_hex_chunked("abc"), "900150983cd24fb0d6963f7d28e17f72", "abc MD5");
    require_equal(md5_hex_chunked("message digest"), "f96b697d7cb7938d525a2f31aaf161d0",
        "message digest MD5");
    require_equal(md5_hex_chunked("abcdefghijklmnopqrstuvwxyz"),
        "c3fcd3d76192e4007dfb496cca67e13b", "alphabet MD5");

    const std::string million_as(1'000'000, 'a');
    require_equal(md5_hex_chunked(million_as), "7707d6ae4e027c70eea2a935c2296f21",
        "million-a MD5");
}

}  // namespace

int main()
{
    try {
        test_md5_vectors();
    } catch (const std::exception& ex) {
        std::cerr << "test_hash: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
