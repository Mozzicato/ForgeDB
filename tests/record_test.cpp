#include "record.h"

#include <gtest/gtest.h>

#include "crc32.h"

using namespace forgedb;

namespace {

Record roundtrip(const Record& in) {
    std::string buf;
    const size_t n = encode_record(in, buf);
    EXPECT_EQ(n, buf.size());
    Record out;
    const DecodeResult r = decode_record(buf, 0, out);
    EXPECT_EQ(r.status, DecodeStatus::kOk);
    EXPECT_EQ(r.size, buf.size());
    return out;
}

// Rewrites the CRC so a deliberately malformed record still passes the
// checksum. Used to test the structural checks behind the CRC.
void fix_crc(std::string& buf) {
    const uint32_t crc = crc32(std::string_view(buf).substr(4));
    for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
}

}  // namespace

TEST(Record, PutRoundTrip) {
    const Record out = roundtrip({42, RecordType::kPut, "user:1", "alice"});
    EXPECT_EQ(out.txn_id, 42u);
    EXPECT_EQ(out.type, RecordType::kPut);
    EXPECT_EQ(out.key, "user:1");
    EXPECT_EQ(out.value, "alice");
}

TEST(Record, DeleteWithEmptyValueRoundTrip) {
    const Record out = roundtrip({7, RecordType::kDelete, "gone", ""});
    EXPECT_EQ(out.type, RecordType::kDelete);
    EXPECT_EQ(out.key, "gone");
    EXPECT_TRUE(out.value.empty());
}

TEST(Record, BinaryKeyAndValueWithZeroBytes) {
    const std::string key("a\0b\xff", 4), value("\0\0\x01", 3);
    const Record out = roundtrip({1, RecordType::kPut, key, value});
    EXPECT_EQ(out.key, key);
    EXPECT_EQ(out.value, value);
}

TEST(Record, OneMegabyteValue) {
    const std::string big(1 << 20, 'x');
    EXPECT_EQ(roundtrip({1, RecordType::kPut, "big", big}).value, big);
}

TEST(Record, EncodedSizeMatchesLayout) {
    std::string buf;
    EXPECT_EQ(encode_record({1, RecordType::kPut, "abc", "12345"}, buf), kMinRecordSize + 3 + 5);
}

TEST(Record, DecodesRecordAtNonZeroOffset) {
    std::string buf;
    const size_t first = encode_record({1, RecordType::kPut, "a", "1"}, buf);
    encode_record({2, RecordType::kPut, "b", "2"}, buf);
    Record out;
    ASSERT_EQ(decode_record(buf, first, out).status, DecodeStatus::kOk);
    EXPECT_EQ(out.key, "b");
}

// A torn write leaves a prefix of the record on disk. Every possible prefix
// must be reported as truncated, never as a valid record.
TEST(Record, EveryProperPrefixIsTruncated) {
    std::string buf;
    encode_record({9, RecordType::kPut, "key", "value"}, buf);
    for (size_t len = 0; len < buf.size(); ++len) {
        Record out;
        EXPECT_EQ(decode_record(std::string_view(buf).substr(0, len), 0, out).status, DecodeStatus::kTruncated)
            << "prefix length " << len;
    }
}

// Flipping any single bit anywhere in the record must be detected. (A flip in
// the length field may show up as "truncated" instead of "corrupt" - either
// way the record is rejected.)
TEST(Record, EverySingleBitFlipIsRejected) {
    std::string buf;
    encode_record({9, RecordType::kPut, "key", "value"}, buf);
    for (size_t i = 0; i < buf.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::string bad = buf;
            bad[i] = static_cast<char>(bad[i] ^ (1 << bit));
            Record out;
            EXPECT_NE(decode_record(bad, 0, out).status, DecodeStatus::kOk) << "byte " << i << " bit " << bit;
        }
    }
}

TEST(Record, KeyLengthInconsistentWithPayloadIsCorruptEvenWithValidCrc) {
    std::string buf;
    encode_record({1, RecordType::kPut, "key", "value"}, buf);
    buf[17] = 100;  // key_len low byte: claims a 100-byte key
    fix_crc(buf);
    Record out;
    EXPECT_EQ(decode_record(buf, 0, out).status, DecodeStatus::kCorrupt);
}

TEST(Record, UnknownTypeIsCorruptEvenWithValidCrc) {
    std::string buf;
    encode_record({1, RecordType::kPut, "key", "value"}, buf);
    buf[16] = 99;  // type byte
    fix_crc(buf);
    Record out;
    EXPECT_EQ(decode_record(buf, 0, out).status, DecodeStatus::kCorrupt);
}

TEST(Record, CommitCountRoundTrip) {
    uint32_t count = 0;
    ASSERT_TRUE(decode_commit_count(encode_commit_count(123456), count));
    EXPECT_EQ(count, 123456u);
    EXPECT_FALSE(decode_commit_count("abc", count));
}
