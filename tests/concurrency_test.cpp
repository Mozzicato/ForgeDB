// Targeted concurrency tests. Each one checks a specific guarantee that a
// locking mistake would break. They are also the main input for the
// ThreadSanitizer CI job.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <latch>
#include <thread>
#include <vector>

#include "db.h"
#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

std::string padded(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08d", i);  // fixed width so string order == numeric order
    return buf;
}

}  // namespace

TEST(Concurrency, ParallelPutsToDistinctKeysAllLandAndPersist) {
    TempDir dir;
    constexpr int kThreads = 8, kPerThread = 500;
    {
        DB db(dir.file("db.fdb"), {SyncMode::kNone});
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&db, t] {
                for (int i = 0; i < kPerThread; ++i) db.put("t" + std::to_string(t) + ":" + std::to_string(i), "v");
            });
        }
        for (auto& th : threads) th.join();
        EXPECT_EQ(db.key_count(), static_cast<size_t>(kThreads * kPerThread));
    }
    DB db(dir.file("db.fdb"));
    EXPECT_EQ(db.recovery_report().stop, ReplayStop::kCleanEnd);
    EXPECT_EQ(db.recovery_report().txns_applied, static_cast<uint64_t>(kThreads * kPerThread));
    EXPECT_EQ(db.key_count(), static_cast<size_t>(kThreads * kPerThread));
}

// remove() is check-then-act. If the check and the write are not under the
// same lock, two threads can both "successfully" remove the same key.
TEST(Concurrency, RacingRemovesOfOneKeySucceedExactlyOnce) {
    TempDir dir;
    DB db(dir.file("db.fdb"), {SyncMode::kNone});
    constexpr int kThreads = 8, kRounds = 200;
    for (int round = 0; round < kRounds; ++round) {
        db.put("k", "v");
        std::atomic<int> successes{0};
        std::latch start(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                start.arrive_and_wait();  // release all threads at once
                if (db.remove("k")) successes++;
            });
        }
        for (auto& th : threads) th.join();
        ASSERT_EQ(successes.load(), 1) << "round " << round;
    }
}

// One writer counts up; readers must never see the value go backwards.
TEST(Concurrency, ReadersSeeMonotonicallyIncreasingValues) {
    TempDir dir;
    DB db(dir.file("db.fdb"), {SyncMode::kNone});
    db.put("counter", padded(0));
    constexpr int kWrites = 3000, kReaders = 4;
    std::atomic<bool> done{false};
    std::atomic<int> violations{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            std::string last = padded(0);
            while (!done.load()) {
                const std::string now = db.get("counter").value();
                if (now < last) violations++;
                last = now;
            }
        });
    }
    for (int i = 1; i <= kWrites; ++i) db.put("counter", padded(i));
    done = true;
    for (auto& th : readers) th.join();
    EXPECT_EQ(violations.load(), 0);
    EXPECT_EQ(db.get("counter"), padded(kWrites));
}

// Each batch sets k0..k49 to the same version i, and k0 is applied first.
// A reader reads k0 and THEN k49. If batches are published atomically, k49
// can only be at the same or a newer version than the k0 it saw. If the
// index were updated key-by-key with readers in between, a reader could see
// the new k0 and the old k49.
TEST(Concurrency, ReadersNeverObserveAPartiallyAppliedBatch) {
    TempDir dir;
    DB db(dir.file("db.fdb"), {SyncMode::kNone});
    constexpr int kKeys = 50, kBatches = 2000, kReaders = 4;
    auto key = [](int k) { return "k" + std::to_string(k); };
    {
        WriteBatch b;
        for (int k = 0; k < kKeys; ++k) b.put(key(k), padded(0));
        db.commit(b);
    }
    std::atomic<bool> done{false};
    std::atomic<int> torn_reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            while (!done.load()) {
                const std::string first = db.get(key(0)).value();
                const std::string last = db.get(key(kKeys - 1)).value();
                if (last < first) torn_reads++;
            }
        });
    }
    for (int i = 1; i <= kBatches; ++i) {
        WriteBatch b;
        for (int k = 0; k < kKeys; ++k) b.put(key(k), padded(i));
        db.commit(b);
    }
    done = true;
    for (auto& th : readers) th.join();
    EXPECT_EQ(torn_reads.load(), 0);
}
