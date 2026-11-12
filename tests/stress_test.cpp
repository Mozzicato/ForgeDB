// Stress test: 50 clients send requests to a pool of 16 worker threads that
// execute them against one shared DB - the shape of an embedded DB inside a
// server. Each client is synchronous (sends a request, waits for the reply),
// like a blocking RPC client, so up to 50 requests are queued and up to 16
// run against the DB at the same time.
//
// Correctness is checked against models that do not depend on thread timing:
//  - Private keys: each client owns 20 keys that only it writes. Every read
//    of an own key must equal the client's model exactly.
//  - Shared keys: everyone writes them. Any value read must be well formed
//    and belong to that key (no torn or mixed-up values).
//  - Pair keys: written only together in a batch with the same tag. Once all
//    clients stop, both must hold the same tag (batches are atomic and
//    serialised).
//  - After closing and reopening, every key must have the same value.
//
// Durability is not under test here (fault tests cover it), so the DB runs
// with SyncMode::kNone to get many more operations per second of test time.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <thread>

#include "db.h"
#include "test_util.h"

using namespace forgedb;
using namespace forgedb::test;

namespace {

constexpr int kWorkers = 16;
constexpr int kClients = 50;
constexpr int kPrivateKeysPerClient = 20;
constexpr int kSharedKeys = 10;

// Fixed-size thread pool: a queue of tasks served by N threads.
class WorkerPool {
public:
    explicit WorkerPool(int n) {
        for (int i = 0; i < n; ++i) threads_.emplace_back([this] { run(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    // Queues `fn` and returns a future that is ready once a worker ran it.
    std::future<void> submit(std::function<void()> fn) {
        std::packaged_task<void()> task(std::move(fn));
        std::future<void> result = task.get_future();
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push(std::move(task));
        }
        cv_.notify_one();
        return result;
    }

private:
    void run() {
        while (true) {
            std::packaged_task<void()> task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping and nothing left
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::packaged_task<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

std::string private_key(int client, int k) { return "c" + std::to_string(client) + ":k" + std::to_string(k); }
std::string shared_key(int k) { return "shared:" + std::to_string(k); }

int ops_per_client() {
    // FORGEDB_STRESS_OPS lets CI or a developer run a longer soak.
    if (const char* env = std::getenv("FORGEDB_STRESS_OPS")) return std::atoi(env);
    return 400;
}

}  // namespace

TEST(Stress, SixteenWorkersFiftyClientsMatchModel) {
    TempDir dir;
    const auto path = dir.file("stress.fdb");
    const int ops = ops_per_client();

    std::atomic<int> errors{0};
    std::mutex error_mu;
    std::string first_error;
    auto fail = [&](const std::string& msg) {
        if (errors++ == 0) {
            std::lock_guard<std::mutex> lock(error_mu);
            first_error = msg;
        }
    };

    // Each client's model of its own keys. Only that client touches its map.
    std::vector<std::map<std::string, std::string>> models(kClients);
    std::map<std::string, std::string> before_close;
    const auto start = std::chrono::steady_clock::now();
    {
        DB db(path, {SyncMode::kNone});
        WorkerPool pool(kWorkers);

        std::vector<std::thread> clients;
        for (int c = 0; c < kClients; ++c) {
            clients.emplace_back([&, c] {
                std::mt19937 rng(1000 + c);  // fixed seed per client: reproducible op mix
                auto& model = models[c];
                for (int seq = 0; seq < ops; ++seq) {
                    const int dice = static_cast<int>(rng() % 100);
                    const std::string own = private_key(c, static_cast<int>(rng() % kPrivateKeysPerClient));
                    const int sk = static_cast<int>(rng() % kSharedKeys);
                    const std::string tag = std::to_string(c) + "#" + std::to_string(seq);
                    std::function<void()> request;

                    if (dice < 35) {  // put own key
                        request = [&, own, tag] {
                            db.put(own, tag);
                            model[own] = tag;
                        };
                    } else if (dice < 45) {  // remove own key
                        request = [&, own] {
                            const bool expected = model.erase(own) == 1;
                            if (db.remove(own) != expected) fail("remove result mismatch for " + own);
                        };
                    } else if (dice < 75) {  // read own key: must match model exactly
                        request = [&, own] {
                            const auto it = model.find(own);
                            const auto want = it == model.end() ? std::nullopt : std::optional(it->second);
                            if (db.get(own) != want) fail("read-your-writes violated for " + own);
                        };
                    } else if (dice < 85) {  // write shared key
                        request = [&, sk, tag] { db.put(shared_key(sk), shared_key(sk) + "=" + tag); };
                    } else if (dice < 95) {  // read shared key: value must belong to that key
                        request = [&, sk] {
                            const auto v = db.get(shared_key(sk));
                            if (v && v->rfind(shared_key(sk) + "=", 0) != 0) fail("foreign value in " + shared_key(sk));
                        };
                    } else {  // atomic batch: both pair keys + an own key
                        request = [&, own, tag] {
                            WriteBatch b;
                            b.put("pair:x", tag);
                            b.put("pair:y", tag);
                            b.put(own, tag);
                            db.commit(b);
                            model[own] = tag;
                        };
                    }
                    // Synchronous client: wait for the reply before the next request.
                    try {
                        pool.submit(std::move(request)).get();
                    } catch (const std::exception& e) {
                        fail(std::string("exception: ") + e.what());
                    }
                }
            });
        }
        for (auto& t : clients) t.join();

        // Quiescent checks.
        EXPECT_EQ(db.get("pair:x"), db.get("pair:y")) << "a batch was not applied atomically";
        for (int c = 0; c < kClients; ++c) {
            for (int k = 0; k < kPrivateKeysPerClient; ++k) {
                const auto it = models[c].find(private_key(c, k));
                const auto want = it == models[c].end() ? std::nullopt : std::optional(it->second);
                EXPECT_EQ(db.get(private_key(c, k)), want);
                if (want) before_close[private_key(c, k)] = *want;
            }
        }
        for (int k = 0; k < kSharedKeys; ++k) {
            if (auto v = db.get(shared_key(k))) before_close[shared_key(k)] = *v;
        }
        if (auto v = db.get("pair:x")) before_close["pair:x"] = *v;
        if (auto v = db.get("pair:y")) before_close["pair:y"] = *v;
        EXPECT_EQ(db.key_count(), before_close.size());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(errors.load(), 0) << "first error: " << first_error;

    // Everything must come back identically after a restart.
    DB reopened(path);
    EXPECT_EQ(reopened.recovery_report().stop, ReplayStop::kCleanEnd);
    EXPECT_EQ(reopened.key_count(), before_close.size());
    for (const auto& [k, v] : before_close) EXPECT_EQ(reopened.get(k), v) << k;

    std::printf("[ stress   ] %d workers, %d clients, %d ops total in %.2f s\n", kWorkers, kClients, kClients * ops,
                secs);
}
