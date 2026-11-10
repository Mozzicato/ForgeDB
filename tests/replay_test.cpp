#include "replay.h"

#include <gtest/gtest.h>

#include <map>

#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

// Replays `log` and returns the committed transactions as (txn_id, keys).
struct Applied {
    std::vector<std::pair<uint64_t, std::vector<std::string>>> txns;
    ReplayReport report;
};

Applied run(const std::string& log) {
    Applied a;
    a.report = replay_log(log, [&](uint64_t id, const std::vector<LoggedOp>& ops) {
        std::vector<std::string> keys;
        for (const auto& op : ops) keys.push_back(op.key);
        a.txns.push_back({id, keys});
    });
    return a;
}

}  // namespace

TEST(Replay, EmptyLogIsCleanAndEmpty) {
    const Applied a = run("");
    EXPECT_EQ(a.report.stop, ReplayStop::kCleanEnd);
    EXPECT_TRUE(a.txns.empty());
    EXPECT_EQ(a.report.valid_bytes, 0u);
}

TEST(Replay, AppliesCommittedTransactionsInOrder) {
    std::string log;
    append_txn(log, 1, {put("a", "1"), put("b", "2")});
    append_txn(log, 2, {del("a")});
    const Applied a = run(log);
    ASSERT_EQ(a.txns.size(), 2u);
    EXPECT_EQ(a.txns[0].first, 1u);
    EXPECT_EQ(a.txns[0].second, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(a.txns[1].second, (std::vector<std::string>{"a"}));
    EXPECT_EQ(a.report.stop, ReplayStop::kCleanEnd);
    EXPECT_EQ(a.report.valid_bytes, log.size());
    EXPECT_EQ(a.report.last_txn_id, 2u);
}

// Complete records but no COMMIT: the process died after writing the ops.
TEST(Replay, UncommittedTailTransactionIsDiscarded) {
    std::string log;
    append_txn(log, 1, {put("a", "1")});
    const size_t committed_end = log.size();
    append_txn(log, 2, {put("b", "2"), put("c", "3")}, /*with_commit=*/false);
    const Applied a = run(log);
    ASSERT_EQ(a.txns.size(), 1u);
    EXPECT_EQ(a.report.txns_discarded, 1u);
    EXPECT_EQ(a.report.valid_bytes, committed_end);
}

TEST(Replay, TornTailStopsAtLastCommit) {
    std::string log;
    append_txn(log, 1, {put("a", "1")});
    const size_t committed_end = log.size();
    append_txn(log, 2, {put("b", "2")});
    log.resize(log.size() - 3);  // cut into txn 2's COMMIT record
    const Applied a = run(log);
    EXPECT_EQ(a.report.stop, ReplayStop::kTornTail);
    ASSERT_EQ(a.txns.size(), 1u);
    EXPECT_EQ(a.report.valid_bytes, committed_end);
}

// Point-in-time recovery: nothing after a corrupt record is applied, even
// later transactions that are themselves intact.
TEST(Replay, CorruptRecordStopsReplayEvenIfLaterRecordsAreIntact) {
    std::string log;
    append_txn(log, 1, {put("a", "1")});
    const size_t txn2_start = log.size();
    append_txn(log, 2, {put("b", "2")});
    append_txn(log, 3, {put("c", "3")});
    log[txn2_start + 20] ^= 0x01;  // flip a bit inside txn 2's PUT
    const Applied a = run(log);
    EXPECT_EQ(a.report.stop, ReplayStop::kCorruptRecord);
    ASSERT_EQ(a.txns.size(), 1u);
    EXPECT_EQ(a.report.valid_bytes, txn2_start);
}

TEST(Replay, CommitCountMismatchIsBadTransaction) {
    std::string log;
    encode_record({1, RecordType::kPut, "a", "1"}, log);
    encode_record({1, RecordType::kCommit, "", encode_commit_count(2)}, log);  // claims 2 ops
    const Applied a = run(log);
    EXPECT_EQ(a.report.stop, ReplayStop::kBadTransaction);
    EXPECT_TRUE(a.txns.empty());
}

TEST(Replay, InterleavedTransactionIdsAreBadTransaction) {
    std::string log;
    encode_record({1, RecordType::kPut, "a", "1"}, log);
    encode_record({2, RecordType::kPut, "b", "2"}, log);
    const Applied a = run(log);
    EXPECT_EQ(a.report.stop, ReplayStop::kBadTransaction);
    EXPECT_TRUE(a.txns.empty());
}

TEST(Replay, NonIncreasingTransactionIdIsBadTransaction) {
    std::string log;
    append_txn(log, 5, {put("a", "1")});
    const size_t first_end = log.size();
    append_txn(log, 5, {put("b", "2")});  // replayed duplicate id
    const Applied a = run(log);
    EXPECT_EQ(a.report.stop, ReplayStop::kBadTransaction);
    EXPECT_EQ(a.txns.size(), 1u);
    EXPECT_EQ(a.report.valid_bytes, first_end);
}

TEST(Replay, CommitWithNoOpsIsBadTransaction) {
    std::string log;
    encode_record({1, RecordType::kCommit, "", encode_commit_count(0)}, log);
    EXPECT_EQ(run(log).report.stop, ReplayStop::kBadTransaction);
}

TEST(Replay, ReportedOffsetsPointAtTheRecords) {
    std::string log;
    append_txn(log, 1, {put("a", "1"), put("bb", "22")});
    std::vector<LoggedOp> seen;
    replay_log(log, [&](uint64_t, const std::vector<LoggedOp>& ops) { seen = ops; });
    ASSERT_EQ(seen.size(), 2u);
    for (const LoggedOp& op : seen) {
        Record r;
        const DecodeResult d = decode_record(log, op.record_offset, r);
        ASSERT_EQ(d.status, DecodeStatus::kOk);
        EXPECT_EQ(d.size, op.record_size);
        EXPECT_EQ(r.key, op.key);
    }
}
