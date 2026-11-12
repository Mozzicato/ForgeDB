#pragma once

// On-disk record format of the ForgeDB log.
//
// The log is a sequence of records. Every record is self-describing:
//
//   offset  size  field
//   0       4     crc32     CRC-32 of bytes [4, 8 + payload_len)  (i.e. len + payload)
//   4       4     len       payload length in bytes
//   8       8     txn_id    id of the transaction this record belongs to
//   16      1     type      RecordType
//   17      4     key_len
//   21      4     value_len
//   25      ...   key bytes, then value bytes
//
// All integers are little-endian, written byte-by-byte so the format does
// not depend on the machine's endianness or struct padding.
//
// The length prefix gives framing (we know where the next record starts);
// the CRC tells us whether the bytes we framed are the bytes we wrote.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace forgedb {

enum class RecordType : uint8_t {
    kPut = 1,
    kDelete = 2,
    // Marks the end of a transaction. Its value holds the number of PUT/DELETE
    // records in the transaction (4 bytes LE). Recovery applies a transaction
    // only after it has seen this record.
    kCommit = 3,
};

struct Record {
    uint64_t txn_id = 0;
    RecordType type = RecordType::kPut;
    std::string key;
    std::string value;
};

inline constexpr size_t kHeaderSize = 8;           // crc + len
inline constexpr size_t kPayloadFixedSize = 17;    // txn_id + type + key_len + value_len
inline constexpr size_t kMinRecordSize = kHeaderSize + kPayloadFixedSize;  // 25

// Appends the encoded record to `out`. Returns the encoded size.
size_t encode_record(const Record& rec, std::string& out);

enum class DecodeStatus {
    kOk,
    kTruncated,  // not enough bytes left for a whole record (torn tail)
    kCorrupt,    // bytes are all there but fail the CRC or are malformed
};

struct DecodeResult {
    DecodeStatus status;
    size_t size = 0;  // encoded size of the record when status == kOk
};

// Decodes the record that starts at `buf[offset]`.
DecodeResult decode_record(std::string_view buf, size_t offset, Record& out);

// Helpers for the 4-byte op count stored in a COMMIT record's value.
std::string encode_commit_count(uint32_t count);
bool decode_commit_count(std::string_view value, uint32_t& count);

}  // namespace forgedb
