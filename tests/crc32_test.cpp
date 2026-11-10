#include "crc32.h"

#include <gtest/gtest.h>

#include <string>

using forgedb::crc32;

// The standard CRC-32 check value: every correct implementation gives this.
TEST(Crc32, MatchesStandardCheckValue) { EXPECT_EQ(crc32("123456789"), 0xCBF43926u); }

TEST(Crc32, EmptyInputIsZero) { EXPECT_EQ(crc32(""), 0u); }

TEST(Crc32, IncrementalEqualsOneShot) {
    const std::string a = "hello, ", b = "forgedb";
    EXPECT_EQ(crc32(b, crc32(a)), crc32(a + b));
}

TEST(Crc32, DetectsEverySingleBitFlip) {
    std::string data = "the quick brown fox jumps over the lazy dog";
    const uint32_t original = crc32(data);
    for (size_t i = 0; i < data.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            data[i] = static_cast<char>(data[i] ^ (1 << bit));
            EXPECT_NE(crc32(data), original) << "byte " << i << " bit " << bit;
            data[i] = static_cast<char>(data[i] ^ (1 << bit));
        }
    }
}
