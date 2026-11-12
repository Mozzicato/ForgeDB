// Transactions: a WriteBatch commits atomically. After a crash at ANY byte of
// the batch's log write, recovery shows either the whole batch or none of it.

#include <gtest/gtest.h>

#include <filesystem>

#include "db.h"
#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

class TxnTest : public ::testing::Test {
protected:
    TempDir dir;
    std::filesystem::path path = dir.file("txn.fdb");
};

void truncate_file(const std::filesystem::path& p, uint64_t size) {
    LogFile f(p);
    f.truncate(size);
}

}  // namespace

TEST_F(TxnTest, BatchCommitsAllKeys) {
    DB db(path);
    WriteBatch b;
    b.put("a", "1");
    b.put("b", "2");
    b.put("c", "3");
    db.commit(b);
    EXPECT_EQ(db.get("a"), "1");
    EXPECT_EQ(db.get("b"), "2");
    EXPECT_EQ(db.get("c"), "3");
}

TEST_F(TxnTest, EmptyBatchWritesNothing) {
    DB db(path);
    db.commit(WriteBatch{});
    EXPECT_EQ(db.log_size(), 0u);
}

// Ops inside one batch are applied in the order they were added, both live
// and when the batch is replayed after a restart.
TEST_F(TxnTest, OpsWithinBatchApplyInOrder) {
    auto check = [](const DB& db) {
        EXPECT_EQ(db.get("a"), "2");
        EXPECT_EQ(db.get("x"), std::nullopt);
    };
    {
        DB db(path);
        WriteBatch b;
        b.put("a", "1");
        b.remove("a");
        b.put("a", "2");
        b.put("x", "tmp");
        b.remove("x");
        db.commit(b);
        check(db);
    }
    DB db(path);
    check(db);
}

TEST_F(TxnTest, BatchIsOneTransactionInTheLog) {
    {
        DB db(path);
        WriteBatch b;
        for (int i = 0; i < 50; ++i) b.put("k" + std::to_string(i), "v");
        db.commit(b);
    }
    DB db(path);
    EXPECT_EQ(db.recovery_report().txns_applied, 1u);
    EXPECT_EQ(db.recovery_report().records_read, 51u);  // 50 PUTs + 1 COMMIT
    EXPECT_EQ(db.key_count(), 50u);
}

TEST_F(TxnTest, BatchCanDeleteKeysWrittenByEarlierTransactions) {
    {
        DB db(path);
        db.put("a", "1");
        db.put("b", "2");
        WriteBatch b;
        b.remove("a");
        b.put("b", "22");
        b.remove("never-existed");
        db.commit(b);
    }
    DB db(path);
    EXPECT_EQ(db.get("a"), std::nullopt);
    EXPECT_EQ(db.get("b"), "22");
    EXPECT_EQ(db.key_count(), 1u);
}

TEST_F(TxnTest, BatchCanBeReusedAfterClear) {
    DB db(path);
    WriteBatch b;
    b.put("a", "1");
    db.commit(b);
    b.clear();
    EXPECT_TRUE(b.empty());
    b.put("b", "2");
    db.commit(b);
    EXPECT_EQ(db.get("a"), "1");
    EXPECT_EQ(db.get("b"), "2");
}

// Losing exactly the COMMIT record (everything else intact) must hide the
// whole batch: the ops alone are not enough to make it visible.
TEST_F(TxnTest, BatchWithoutCommitRecordIsInvisible) {
    uint64_t before;
    {
        DB db(path);
        db.put("base", "0");
        before = db.log_size();
        WriteBatch b;
        b.put("a", "1");
        b.put("b", "2");
        db.commit(b);
    }
    // The COMMIT record is the last kMinRecordSize + 4 bytes (empty key, 4-byte count).
    truncate_file(path, std::filesystem::file_size(path) - (kMinRecordSize + 4));
    DB db(path);
    EXPECT_EQ(db.get("base"), "0");
    EXPECT_EQ(db.get("a"), std::nullopt);
    EXPECT_EQ(db.get("b"), std::nullopt);
    EXPECT_EQ(db.recovery_report().txns_discarded, 1u);
    EXPECT_EQ(db.log_size(), before);
}

// The core atomicity check: simulate a crash at every byte boundary inside
// the batch's write. At each cut, the state must be exactly the state before
// the batch - never a partial batch.
TEST_F(TxnTest, CrashAtEveryByteOfBatchShowsAllOrNothing) {
    uint64_t before, after;
    {
        DB db(path);
        db.put("a", "old-a");
        db.put("b", "old-b");
        before = db.log_size();
        WriteBatch b;
        b.put("a", "new-a");
        b.remove("b");
        b.put("c", "new-c");
        db.commit(b);
        after = db.log_size();
    }
    const std::string full = LogFile(path).read_all();

    for (uint64_t cut = before; cut < after; ++cut) {
        {
            LogFile f(path);
            f.truncate(0);
            f.write_at(0, std::string_view(full).substr(0, cut));
        }
        DB db(path);
        EXPECT_EQ(db.get("a"), "old-a") << "cut at " << cut;
        EXPECT_EQ(db.get("b"), "old-b") << "cut at " << cut;
        EXPECT_EQ(db.get("c"), std::nullopt) << "cut at " << cut;
    }
    // And with every byte present, the whole batch is visible.
    {
        LogFile f(path);
        f.write_at(0, full);
    }
    DB db(path);
    EXPECT_EQ(db.get("a"), "new-a");
    EXPECT_EQ(db.get("b"), std::nullopt);
    EXPECT_EQ(db.get("c"), "new-c");
}

TEST_F(TxnTest, LargeBatchTornInTheMiddleLeavesNoKeys) {
    constexpr int kOps = 1000;
    uint64_t before, after;
    {
        DB db(path, {SyncMode::kNone});
        db.put("sentinel", "s");
        before = db.log_size();
        WriteBatch b;
        for (int i = 0; i < kOps; ++i) b.put("key" + std::to_string(i), std::string(100, 'v'));
        db.commit(b);
        after = db.log_size();
    }
    truncate_file(path, before + (after - before) / 2);
    DB db(path);
    EXPECT_EQ(db.key_count(), 1u);
    EXPECT_EQ(db.get("sentinel"), "s");
    for (int i = 0; i < kOps; i += 50) EXPECT_EQ(db.get("key" + std::to_string(i)), std::nullopt);
}
