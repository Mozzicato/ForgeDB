#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "record.h"

namespace forgedb::test {

// A fresh, empty directory that is deleted when the object goes away.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("forgedb_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 std::to_string(counter++) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

struct Op {
    RecordType type;
    std::string key;
    std::string value;
};

// Encodes one transaction (its ops followed by a COMMIT) the same way the
// engine does. `with_commit = false` leaves the transaction open.
inline void append_txn(std::string& log, uint64_t txn_id, const std::vector<Op>& ops, bool with_commit = true) {
    for (const Op& op : ops) encode_record({txn_id, op.type, op.key, op.value}, log);
    if (with_commit) {
        encode_record({txn_id, RecordType::kCommit, "", encode_commit_count(static_cast<uint32_t>(ops.size()))}, log);
    }
}

inline Op put(std::string k, std::string v) { return {RecordType::kPut, std::move(k), std::move(v)}; }
inline Op del(std::string k) { return {RecordType::kDelete, std::move(k), ""}; }

}  // namespace forgedb::test
