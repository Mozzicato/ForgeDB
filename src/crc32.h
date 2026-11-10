#pragma once

#include <cstdint>
#include <string_view>

namespace forgedb {

// CRC-32 (IEEE 802.3, the same polynomial as zlib/PNG/Ethernet).
// Used to detect torn writes and bit-flips in log records.
// `seed` lets a caller checksum several pieces as if they were one buffer:
//   crc32(b, crc32(a)) == crc32(a + b)
uint32_t crc32(std::string_view data, uint32_t seed = 0);

}  // namespace forgedb
