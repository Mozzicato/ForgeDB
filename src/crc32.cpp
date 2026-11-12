#include "crc32.h"

#include <array>

namespace forgedb {
namespace {

// Byte-at-a-time table for the reflected polynomial 0xEDB88320.
// Built at compile time so there is no "first call" initialisation race.
constexpr std::array<uint32_t, 256> make_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kTable = make_table();

}  // namespace

uint32_t crc32(std::string_view data, uint32_t seed) {
    uint32_t c = ~seed;
    for (unsigned char byte : data) {
        c = kTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace forgedb
