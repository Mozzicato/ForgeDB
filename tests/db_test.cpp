#include "db.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

void flip_byte(const std::filesystem::path& p, uint64_t offset) {
    LogFile f(p);
    std::string b = f.read_at(offset, 1);
    b[0] = static_cast<char>(b[0] ^ 0x5A);
    f.write_at(offset, b);
}

void append_bytes(const std::filesystem::path& p, std::string_view data) {
    LogFile f(p);
    f.write_at(f.size(), data);
}

class DBTest : public ::testing::Test {
protected:
    TempDir dir;
    std::filesystem::path path = dir.file("test.fdb");
};

}  // namespace

TEST_F(DBTest, PutThenGet) {
    DB db(path);
    db.put("k", "v");
    EXPECT_EQ(db.get("k"), "v");
}

TEST_F(DBTest, GetMissingKeyReturnsNullopt) {
    DB db(path);
    EXPECT_EQ(db.get("nope"), std::nullopt);
}

TEST_F(DBTest, OverwriteReturnsLatestValue) {
    DB db(path);
    db.put("k", "v1");
    db.put("k", "v2");
    EXPECT_EQ(db.get("k"), "v2");
    EXPECT_EQ(db.key_count(), 1u);
}

TEST_F(DBTest, RemoveDeletesKey) {
    DB db(path);
    db.put("k", "v");
    EXPECT_TRUE(db.remove("k"));
    EXPECT_EQ(db.get("k"), std::nullopt);
    EXPECT_EQ(db.key_count(), 0u);
}

TEST_F(DBTest, RemoveMissingKeyReturnsFalseAndWritesNothing) {
    DB db(path);
    db.put("a", "1");
    const uint64_t before = db.log_size();
    EXPECT_FALSE(db.remove("b"));
    EXPECT_EQ(db.log_size(), before);
}

TEST_F(DBTest, EmptyValueIsDistinctFromMissing) {
    DB db(path);
    db.put("k", "");
    EXPECT_EQ(db.get("k"), "");
}

TEST_F(DBTest, DataPersistsAcrossReopen) {
    {
        DB db(path);
        db.put("a", "1");
        db.put("b", "2");
    }
    DB db(path);
    EXPECT_EQ(db.get("a"), "1");
    EXPECT_EQ(db.get("b"), "2");
    EXPECT_EQ(db.recovery_report().stop, ReplayStop::kCleanEnd);
}

TEST_F(DBTest, OverwriteAndDeletePersistAcrossReopen) {
    {
        DB db(path);
        db.put("a", "old");
        db.put("a", "new");
        db.put("b", "x");
        db.remove("b");
    }
    DB db(path);
    EXPECT_EQ(db.get("a"), "new");
    EXPECT_EQ(db.get("b"), std::nullopt);
    EXPECT_EQ(db.key_count(), 1u);
}

TEST_F(DBTest, ManyKeysSurviveReopen) {
    constexpr int kKeys = 10000;
    {
        DB db(path, {SyncMode::kNone});
        for (int i = 0; i < kKeys; ++i) db.put("key" + std::to_string(i), "val" + std::to_string(i));
    }
    DB db(path);
    ASSERT_EQ(db.key_count(), static_cast<size_t>(kKeys));
    for (int i = 0; i < kKeys; i += 97) EXPECT_EQ(db.get("key" + std::to_string(i)), "val" + std::to_string(i));
    EXPECT_EQ(db.recovery_report().txns_applied, static_cast<uint64_t>(kKeys));
}

// The torn tail must be cut off on open; otherwise writes made after the
// restart would sit behind garbage and be lost on the NEXT restart.
TEST_F(DBTest, TornTailIsTruncatedSoLaterWritesSurvive) {
    uint64_t good_size;
    {
        DB db(path);
        db.put("a", "1");
        good_size = db.log_size();
    }
    append_bytes(path, std::string("\x10\x20\x30\x40\x50", 5));  // half a record header
    {
        DB db(path);
        EXPECT_EQ(db.recovery_report().stop, ReplayStop::kTornTail);
        EXPECT_EQ(db.log_size(), good_size);
        EXPECT_EQ(std::filesystem::file_size(path), good_size);
        db.put("b", "2");
    }
    DB db(path);
    EXPECT_EQ(db.recovery_report().stop, ReplayStop::kCleanEnd);
    EXPECT_EQ(db.get("a"), "1");
    EXPECT_EQ(db.get("b"), "2");
}

TEST_F(DBTest, CorruptRecordRecoversStateBeforeIt) {
    uint64_t second_txn_start;
    {
        DB db(path);
        db.put("a", "1");
        second_txn_start = db.log_size();
        db.put("b", "2");
        db.put("c", "3");
    }
    flip_byte(path, second_txn_start + 30);  // inside b's key/value
    DB db(path);
    EXPECT_EQ(db.recovery_report().stop, ReplayStop::kCorruptRecord);
    EXPECT_EQ(db.get("a"), "1");
    EXPECT_EQ(db.get("b"), std::nullopt);
    EXPECT_EQ(db.get("c"), std::nullopt);  // point-in-time: nothing after the damage
    EXPECT_EQ(db.log_size(), second_txn_start);
}

// Damage that happens while the DB is open is caught by the CRC check in get().
TEST_F(DBTest, GetDetectsCorruptionOfRecordOnDisk) {
    DB db(path);
    db.put("k", "value-on-disk");
    flip_byte(path, db.log_size() - 30);  // inside k's record, before the COMMIT record
    EXPECT_THROW(db.get("k"), CorruptionError);
}

// Transaction ids must keep increasing across restarts, or replay would
// reject the post-restart transactions as out of order.
TEST_F(DBTest, TransactionIdsContinueAcrossReopen) {
    {
        DB db(path);
        db.put("a", "1");
    }
    {
        DB db(path);
        db.put("b", "2");
    }
    DB db(path);
    EXPECT_EQ(db.recovery_report().stop, ReplayStop::kCleanEnd);
    EXPECT_EQ(db.recovery_report().last_txn_id, 2u);
    EXPECT_EQ(db.get("b"), "2");
}

TEST_F(DBTest, ReopenEmptyFileIsEmptyDatabase) {
    { DB db(path); }
    DB db(path);
    EXPECT_EQ(db.key_count(), 0u);
    EXPECT_EQ(db.log_size(), 0u);
}

TEST_F(DBTest, BinaryKeysAndLargeValues) {
    const std::string key("\0k\xff", 3), big(2 << 20, 'z');
    {
        DB db(path);
        db.put(key, big);
    }
    DB db(path);
    EXPECT_EQ(db.get(key), big);
}
