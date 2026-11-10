#include "record.h"

#include "crc32.h"

namespace forgedb {
namespace {

void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint32_t get_u32(std::string_view buf, size_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(buf[pos + i])) << (8 * i);
    return v;
}

uint64_t get_u64(std::string_view buf, size_t pos) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(buf[pos + i])) << (8 * i);
    return v;
}

bool is_known_type(uint8_t t) {
    return t == static_cast<uint8_t>(RecordType::kPut) || t == static_cast<uint8_t>(RecordType::kDelete) ||
           t == static_cast<uint8_t>(RecordType::kCommit);
}

}  // namespace

size_t encode_record(const Record& rec, std::string& out) {
    const size_t start = out.size();
    const uint32_t payload_len = static_cast<uint32_t>(kPayloadFixedSize + rec.key.size() + rec.value.size());

    put_u32(out, 0);  // crc placeholder, filled in below
    put_u32(out, payload_len);
    put_u64(out, rec.txn_id);
    out.push_back(static_cast<char>(rec.type));
    put_u32(out, static_cast<uint32_t>(rec.key.size()));
    put_u32(out, static_cast<uint32_t>(rec.value.size()));
    out.append(rec.key);
    out.append(rec.value);

    // CRC covers the length field too, so a flipped length is caught.
    const uint32_t crc = crc32(std::string_view(out).substr(start + 4));
    for (int i = 0; i < 4; ++i) out[start + i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
    return out.size() - start;
}

DecodeResult decode_record(std::string_view buf, size_t offset, Record& out) {
    if (offset > buf.size()) return {DecodeStatus::kTruncated};
    const size_t remaining = buf.size() - offset;
    if (remaining < kHeaderSize) return {DecodeStatus::kTruncated};

    const uint32_t stored_crc = get_u32(buf, offset);
    const uint32_t payload_len = get_u32(buf, offset + 4);

    // A record whose declared length runs past the end of the file is a torn
    // tail write. (It could also be a corrupted length field - we cannot tell
    // the two apart, and recovery treats both the same way: stop here.)
    if (payload_len > remaining - kHeaderSize) return {DecodeStatus::kTruncated};

    const size_t record_size = kHeaderSize + payload_len;
    if (crc32(buf.substr(offset + 4, 4 + payload_len)) != stored_crc) return {DecodeStatus::kCorrupt};

    // CRC matched; still validate the structure so a buggy writer cannot
    // make us read out of bounds.
    if (payload_len < kPayloadFixedSize) return {DecodeStatus::kCorrupt};
    const size_t p = offset + kHeaderSize;
    const uint8_t type = static_cast<uint8_t>(buf[p + 8]);
    const uint32_t key_len = get_u32(buf, p + 9);
    const uint32_t value_len = get_u32(buf, p + 13);
    if (!is_known_type(type)) return {DecodeStatus::kCorrupt};
    if (static_cast<uint64_t>(kPayloadFixedSize) + key_len + value_len != payload_len) {
        return {DecodeStatus::kCorrupt};
    }

    out.txn_id = get_u64(buf, p);
    out.type = static_cast<RecordType>(type);
    out.key.assign(buf.substr(p + kPayloadFixedSize, key_len));
    out.value.assign(buf.substr(p + kPayloadFixedSize + key_len, value_len));
    return {DecodeStatus::kOk, record_size};
}

std::string encode_commit_count(uint32_t count) {
    std::string s;
    put_u32(s, count);
    return s;
}

bool decode_commit_count(std::string_view value, uint32_t& count) {
    if (value.size() != 4) return false;
    count = get_u32(value, 0);
    return true;
}

}  // namespace forgedb
