// Fault-injection suite. Scenarios are GENERATED from a log produced by the
// real engine, not written by hand:
//
//   1. Truncate the log at every byte offset          (torn write / lost tail)
//   2. Flip one byte in every field of every record   (bit rot, bad sector)
//   3. Zero a 512-byte sector at every record start   (lost sector)
//   4. Append garbage after the last commit           (pre-allocated / junk tail)
//   5. Kill the process at each commit stage          (real process death)
//   6. Fail the write/fsync with an I/O error         (fsync failure handling)
//
// The oracle is a std::map that applies the same transactions. For a log
// damaged at byte offset X, the expected state is the state after the last
// transaction whose COMMIT record ends at or before X. After recovery, each
// scenario also writes a new key and reopens, to prove the database is
// usable again and that recovery truncated the damage.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

#include "db.h"
#include "fault.h"
#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

using State = std::map<std::string, std::string>;

// --- deterministic workload -----------------------------------------------

// Ops of transaction number i (1-based). Mixes single puts, overwrites,
// deletes, empty values and multi-key batches.
std::vector<Op> txn_ops(int i) {
    switch (i % 4) {
        case 0: return {put("k" + std::to_string(i % 3), "v" + std::to_string(i))};       // overwrite
        case 1: return {put("a" + std::to_string(i), "batch"), put("b" + std::to_string(i), ""),
                        del("k" + std::to_string((i + 1) % 3))};                            // batch
        case 2: return {del("a" + std::to_string(i - 1))};                                  // delete
        default: return {put("k" + std::to_string(i % 3), std::string(i, 'x'))};           // longer value
    }
}

void apply_ops(State& s, const std::vector<Op>& ops) {
    for (const Op& op : ops) {
        if (op.type == RecordType::kPut) s[op.key] = op.value;
        else s.erase(op.key);
    }
}

void commit_ops(DB& db, const std::vector<Op>& ops) {
    WriteBatch b;
    for (const Op& op : ops) {
        if (op.type == RecordType::kPut) b.put(op.key, op.value);
        else b.remove(op.key);
    }
    db.commit(b);
}

struct Workload {
    std::string log;                      // bytes written by the real engine
    std::vector<uint64_t> txn_end;        // txn_end[i] = log offset where txn i+1's COMMIT ends
    std::vector<State> state_after;       // state_after[n] = state after the first n txns
    std::set<std::string> keys;           // every key ever touched
    std::vector<uint64_t> record_starts;  // offset of every record in the log
};

Workload build_workload(const std::filesystem::path& path, int txns) {
    Workload w;
    w.state_after.push_back({});
    {
        DB db(path, {SyncMode::kNone});
        for (int i = 1; i <= txns; ++i) {
            const auto ops = txn_ops(i);
            commit_ops(db, ops);
            State next = w.state_after.back();
            apply_ops(next, ops);
            w.state_after.push_back(next);
            w.txn_end.push_back(db.log_size());
            for (const Op& op : ops) w.keys.insert(op.key);
        }
    }
    w.log = LogFile(path).read_all();
    Record rec;
    for (size_t off = 0; off < w.log.size();) {
        w.record_starts.push_back(off);
        off += decode_record(w.log, off, rec).size;
    }
    return w;
}

// State the oracle expects if the log is damaged starting at `offset`.
const State& expected_if_damaged_at(const Workload& w, uint64_t offset) {
    size_t n = 0;
    while (n < w.txn_end.size() && w.txn_end[n] <= offset) ++n;
    return w.state_after[n];
}

// Returns "" if the DB matches `expected`, otherwise a description.
std::string diff(const DB& db, const State& expected, const std::set<std::string>& keys) {
    if (db.key_count() != expected.size()) {
        return "key_count " + std::to_string(db.key_count()) + " != " + std::to_string(expected.size());
    }
    for (const std::string& k : keys) {
        const auto it = expected.find(k);
        const auto want = it == expected.end() ? std::nullopt : std::optional(it->second);
        if (db.get(k) != want) return "wrong value for key '" + k + "'";
    }
    return "";
}

// Writes `bytes` as the log, recovers, checks the oracle, then writes once
// more and reopens to check the DB is healthy. Returns "" on success.
std::string run_scenario(const std::filesystem::path& path, std::string_view bytes, const State& expected,
                         const std::set<std::string>& keys) {
    {
        LogFile f(path);
        f.truncate(0);
        f.write_at(0, bytes);
    }
    State after = expected;
    after["post-recovery"] = "ok";
    std::set<std::string> keys2 = keys;
    keys2.insert("post-recovery");
    try {
        {
            DB db(path, {SyncMode::kNone});
            if (auto d = diff(db, expected, keys); !d.empty()) return "after recovery: " + d;
            db.put("post-recovery", "ok");
        }
        DB db(path, {SyncMode::kNone});
        if (db.recovery_report().stop != ReplayStop::kCleanEnd) return "second reopen not clean";
        if (auto d = diff(db, after, keys2); !d.empty()) return "after second reopen: " + d;
    } catch (const std::exception& e) {
        return std::string("exception: ") + e.what();
    }
    return "";
}

class FaultSuite : public ::testing::Test {
protected:
    static constexpr int kTxns = 12;
    TempDir dir;
    std::filesystem::path path = dir.file("fault.fdb");
    Workload w = build_workload(dir.file("source.fdb"), kTxns);

    void report(const char* category, int scenarios, const std::vector<std::string>& failures) {
        std::printf("[ faults   ] %-38s %5d scenarios, %zu failed\n", category, scenarios, failures.size());
        for (size_t i = 0; i < failures.size() && i < 10; ++i) ADD_FAILURE() << failures[i];
    }
};

}  // namespace

TEST_F(FaultSuite, TruncateAtEveryByteOffset) {
    std::vector<std::string> failures;
    int n = 0;
    for (uint64_t cut = 0; cut <= w.log.size(); ++cut, ++n) {
        const auto err = run_scenario(path, std::string_view(w.log).substr(0, cut), expected_if_damaged_at(w, cut), w.keys);
        if (!err.empty()) failures.push_back("cut=" + std::to_string(cut) + ": " + err);
    }
    report("truncate at every byte", n, failures);
    EXPECT_GT(n, 100);
}

TEST_F(FaultSuite, FlipOneByteInEveryFieldOfEveryRecord) {
    struct Field {
        const char* name;
        size_t offset;  // within the record
        size_t size;
    };
    std::vector<std::string> failures;
    int n = 0;
    Record rec;
    for (const uint64_t start : w.record_starts) {
        decode_record(w.log, start, rec);
        const std::vector<Field> fields = {
            {"crc", 0, 4},       {"len", 4, 4},       {"txn_id", 8, 8},
            {"type", 16, 1},     {"key_len", 17, 4},  {"value_len", 21, 4},
            {"key", 25, rec.key.size()}, {"value", 25 + rec.key.size(), rec.value.size()},
        };
        for (const Field& f : fields) {
            if (f.size == 0) continue;  // e.g. COMMIT records have no key
            for (const unsigned char mask : {0x01, 0x80}) {
                std::string bad = w.log;
                bad[start + f.offset + f.size / 2] ^= static_cast<char>(mask);
                const auto err = run_scenario(path, bad, expected_if_damaged_at(w, start), w.keys);
                if (!err.empty()) {
                    failures.push_back("record@" + std::to_string(start) + " field " + f.name + ": " + err);
                }
                ++n;
            }
        }
    }
    report("flip byte in every field of every record", n, failures);
}

TEST_F(FaultSuite, ZeroA512ByteSectorAtEveryRecordStart) {
    std::vector<std::string> failures;
    int n = 0;
    for (const uint64_t start : w.record_starts) {
        std::string bad = w.log;
        const size_t len = std::min<size_t>(512, bad.size() - start);
        std::fill(bad.begin() + static_cast<std::ptrdiff_t>(start),
                  bad.begin() + static_cast<std::ptrdiff_t>(start + len), '\0');
        const auto err = run_scenario(path, bad, expected_if_damaged_at(w, start), w.keys);
        if (!err.empty()) failures.push_back("zeroed@" + std::to_string(start) + ": " + err);
        ++n;
    }
    report("zero 512-byte sector at record start", n, failures);
}

// Damage every record of one whole transaction (CRC field only, so lengths
// stay readable) while later transactions remain intact. A recovery that
// skipped bad records would resynchronise on the next transaction and apply
// it on top of a hole. Point-in-time recovery must stop before the damage.
TEST_F(FaultSuite, CorruptEveryRecordOfOneTransaction) {
    std::vector<std::string> failures;
    int n = 0;
    for (size_t t = 0; t < w.txn_end.size(); ++t) {
        const uint64_t txn_start = t == 0 ? 0 : w.txn_end[t - 1];
        std::string bad = w.log;
        for (const uint64_t start : w.record_starts) {
            if (start >= txn_start && start < w.txn_end[t]) bad[start + 1] ^= 0x01;
        }
        const auto err = run_scenario(path, bad, w.state_after[t], w.keys);
        if (!err.empty()) failures.push_back("txn " + std::to_string(t + 1) + ": " + err);
        ++n;
    }
    report("corrupt every record of one txn", n, failures);
}

TEST_F(FaultSuite, GarbageAppendedAfterLastCommit) {
    std::vector<std::string> failures;
    int n = 0;
    std::mt19937 rng(7);
    for (const size_t len : {1, 7, 8, 24, 25, 100, 4096}) {
        for (const bool zeros : {true, false}) {
            std::string bad = w.log;
            for (size_t i = 0; i < len; ++i) bad.push_back(zeros ? '\0' : static_cast<char>(rng() & 0xFF));
            const auto err = run_scenario(path, bad, w.state_after.back(), w.keys);
            if (!err.empty()) failures.push_back(std::to_string(len) + (zeros ? " zero" : " random") + " bytes: " + err);
            ++n;
        }
    }
    report("garbage appended after last commit", n, failures);
}

// --- real process death -----------------------------------------------------

namespace {

constexpr int kKilledExitCode = 77;
fault::Point g_kill_point;
int g_commits_until_kill = 0;

void kill_hook(fault::Point p) {
    if (p == g_kill_point && --g_commits_until_kill == 0) std::_Exit(kKilledExitCode);
}

struct KillCase {
    fault::Point point;
    int kill_at_txn;  // the process dies during this transaction
};

std::string point_name(fault::Point p) {
    switch (p) {
        case fault::Point::kBeforeWrite: return "BeforeWrite";
        case fault::Point::kAfterWriteBeforeSync: return "AfterWriteBeforeSync";
        case fault::Point::kAfterSyncBeforePublish: return "AfterSyncBeforePublish";
    }
    return "?";
}

class ProcessKill : public ::testing::TestWithParam<KillCase> {};

}  // namespace

// A child process commits transactions 1..k and is killed (std::_Exit, no
// cleanup, no destructors) at the chosen point inside transaction k. The
// parent then recovers and checks the state.
//
// Expected:
//   BeforeWrite            -> txns 1..k-1      (txn k never reached the file)
//   AfterWriteBeforeSync   -> txns 1..k        (process death does not lose data
//                                               the OS already has in its page
//                                               cache; only power loss would)
//   AfterSyncBeforePublish -> txns 1..k        (durable but never acknowledged:
//                                               allowed to appear)
TEST_P(ProcessKill, RecoveryMatchesOracle) {
    const KillCase c = GetParam();
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& ch : name) if (ch == '/') ch = '_';
    TempDir dir(name);
    const auto path = dir.file("killed.fdb");

    EXPECT_EXIT(
        {
            DB db(path, {SyncMode::kEveryCommit});
            g_kill_point = c.point;
            g_commits_until_kill = c.kill_at_txn;
            fault::set_hook(kill_hook);
            for (int i = 1; i <= c.kill_at_txn; ++i) commit_ops(db, txn_ops(i));
            std::_Exit(0);  // not reached if the kill fired
        },
        ::testing::ExitedWithCode(kKilledExitCode), "");

    State expected;
    std::set<std::string> keys;
    const int survived = c.point == fault::Point::kBeforeWrite ? c.kill_at_txn - 1 : c.kill_at_txn;
    for (int i = 1; i <= c.kill_at_txn; ++i) {
        for (const Op& op : txn_ops(i)) keys.insert(op.key);
        if (i <= survived) apply_ops(expected, txn_ops(i));
    }
    DB db(path);
    EXPECT_EQ(diff(db, expected, keys), "");
    EXPECT_EQ(db.recovery_report().last_txn_id, static_cast<uint64_t>(survived));
}

INSTANTIATE_TEST_SUITE_P(
    AllCommitStages, ProcessKill,
    ::testing::ValuesIn([] {
        std::vector<KillCase> cases;
        for (auto p : {fault::Point::kBeforeWrite, fault::Point::kAfterWriteBeforeSync,
                       fault::Point::kAfterSyncBeforePublish}) {
            for (int k : {1, 2, 5, 8, 12}) cases.push_back({p, k});  // single puts and batches (see txn_ops)
        }
        return cases;
    }()),
    [](const ::testing::TestParamInfo<KillCase>& info) {
        return point_name(info.param.point) + "_txn" + std::to_string(info.param.kill_at_txn);
    });

// --- I/O errors ----------------------------------------------------------------

namespace {
void throw_after_write(fault::Point p) {
    if (p == fault::Point::kAfterWriteBeforeSync) throw IoError("injected fsync failure");
}
void throw_before_write(fault::Point p) {
    if (p == fault::Point::kBeforeWrite) throw IoError("injected failure before write");
}
struct HookGuard {
    explicit HookGuard(fault::Hook h) { fault::set_hook(h); }
    ~HookGuard() { fault::set_hook(nullptr); }
};
}  // namespace

// After a failed fsync we cannot know what is on disk, so the DB must refuse
// further writes until it is reopened (and recovery re-reads the truth).
TEST(FaultIo, FailedSyncMakesDatabaseRefuseWrites) {
    TempDir dir;
    DB db(dir.file("db.fdb"));
    db.put("before", "1");
    {
        HookGuard guard(throw_after_write);
        EXPECT_THROW(db.put("x", "2"), IoError);
    }
    EXPECT_THROW(db.put("after", "3"), IoError);  // still refused with no fault active
    EXPECT_EQ(db.get("before"), "1");              // reads keep working
    EXPECT_EQ(db.get("x"), std::nullopt);          // the failed write was never published
}

TEST(FaultIo, ReopenAfterFailedSyncRecoversAndAcceptsWrites) {
    TempDir dir;
    {
        DB db(dir.file("db.fdb"));
        db.put("before", "1");
        HookGuard guard(throw_after_write);
        EXPECT_THROW(db.put("x", "2"), IoError);
    }
    DB db(dir.file("db.fdb"));
    EXPECT_EQ(db.get("before"), "1");
    db.put("after", "3");
    EXPECT_EQ(db.get("after"), "3");
}

// A failure before anything was written leaves no trace and does not
// disable the DB.
TEST(FaultIo, FailureBeforeWriteLeavesLogUnchanged) {
    TempDir dir;
    DB db(dir.file("db.fdb"));
    db.put("a", "1");
    const uint64_t size = db.log_size();
    {
        HookGuard guard(throw_before_write);
        EXPECT_THROW(db.put("b", "2"), IoError);
    }
    EXPECT_EQ(db.log_size(), size);
    db.put("c", "3");
    EXPECT_EQ(db.get("c"), "3");
}
